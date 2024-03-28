# LSVD/RBD Architecture

THESE ARE FROM LONG AGO. They are probably wrong and are kept only for ease of
access to historical decisions.

## Storage Format

**Local storage** - the read and write caches are stored in a single fixed-length file in the local file system. At present the read and write caches don't share any data, so in theory they could be split in two; however we're trying to figure out how to unify them.

The original version used disk partitions for local storage, because the kernel component (a device mapper) required that. In a user-space library it's just as easy to access regular files, making configuration a lot easier, probably with no effect on performance.

**Back end** - a disk image has a "superblock", e.g. `diskname`, and a series of data and checkpoint objects named `diskname.00000000`, `diskname.00000001` etc. We're currently using 32-bit sequence numbers (8 hex digits), however we might want to go with larger ones. (writing 8MB, this allows 55PB to be written to a volume)

The garbage collector works on large batches to get low write amplification, and reduces its memory footprint by uses the local file system for temporary storage. We've thought of putting the data in the read cache, but that's a lot more complicated than just writing a file.

## Object instances

An open LSVD image is made ob of the following object instances:
* `lsvd_image` - main object
    Besides grouping everything, this includes the following shared state:
	- object map: maps from LBA to object/offset, with associated `shared_mutex`. Used by translation layer, garbage collector, and read cache
	- object info: maps from object number to size and live sector count. Protected by same mutex as map
	- event socket and RBD completion queue
* `translate` - object translation layer. Note that this is write-only; reads are done by the read cache
* `write_cache` - maintains the journaled write cache 
* `read_cache` - read logic, including local read cache
* `cleaner` - garbage collection
* `config` - configuration file stuff (**not implemented yet**)

Helper classes:
* `request` - generic structure for notification up and down the stack
* `backend` - either `rados_backend` or `file_backend`, reads and writes objects
* `nvme` - request-based read/write to local files, currently using `libaio` (maybe use `io_uring` instead?)

Formats for the backend-objects and the local cache are found in `objects.h` and `journal.h` respectively. Currently there's a helper class for handling object headers (`object_parser`) but none for the cache format.

There's a whole bunch of debug code at the end of `lsvd.cc`, which in turn gets used along with `lsvd.py` and `lsvd_types.py` to implement unit tests using the Python `unittest` framework.
This stuff really needs to be cleaned up:
- Unit tests should probably be C++, not Python
- Mock components and call the standard API when possible instead of providing direct access to component methods
- Needs a whole bunch more tests

Note that the code currently uses iovecs for passing data between layers, because there are a number of places where that avoids lots of copying. Unfortunately that means an extra copy of user data when using the RADOS C API - maybe we could use the C++ API and convert everything to bufferlists. Actually we sort of use iovecs - there's a `smartiov` helper class which wraps them.

Most of the code works with LBAs of 512-byte sectors rather than byte offsets - it might clean things up a little bit to use byte offsets in some of those places, although some of the maps (write cache, main LBA-to-object map) should remain in units of sectors.

A number of classes - write cache, read cache, nvme I/O, translation layer, cleaner - use polymorphism with a single implementation subclass. 

## Async request handling

Async write requests go through the following path:
- LSVD/RBD request (`rbd_completion_t` is a pointer to this)
    - write cache request (this can be a batch of multiple API-level writes)
	    - NVME request - gets submitted to libaio
    - translation layer request - gets copied to a batch buffer
	    - object write request (batched writes - currently 8MB)

Notes:
- writes are completed at the API level after submitting to the translation layer, and are written to the backend asynchronously
- there are separate windows for the amount of data in flight to  (a) the write cache, and (b) the backend. `rbd_aio_write` will stall if the window is full.

Async read requests:
- LSVD/RBD request
    - `write_cache` request
	    - nvme request
    - `read_cache` request
	    - cache miss:
		    - backend read request
			- nvme write (async)
		- cache bypass:
		    - backend read request
	    - cache hit:
		    - nvme request

Notes:
- (see below) if the map is fragmented, a single read can result in (a) NVME reads from the write cache, (b) NVME reads from the read cache, and (c) object reads from the backend
- the read cache fetches fairly large "cache lines" (currently 64K) from the backend, and  is inefficient for small random reads. If the cache is missing too much, reads will bypass it.

Async reads are hairy because they can launch multiple async I/Os, potentially across two destinations - the write and read caches.
To deal with it, the write cache and read cache both implement the following interface:
```
  [len1,len2] = async_read(LBA, len)
```
Indicating that:
1. `len1` sectors starting at LBA were not found in the map
2. (if `len2 > 0`) an async operation was launched for `len2` sectors starting at `LBA+len1`
Note that `len1+len2` may be less than the original length, so we end up with the following loop:
```
    while len > 0:
	    [len1,len2] = write_cache->async_read(LBA, len)
		while len1 > 0:
		    [len3,len4] = read_cache->async_read(LBA, len1)
			zero out buf[0..len3]
			LBA += (len3+len4) 
			len1 -= (len3+len4)
		LBA += len2
		len -= len2
```

I guess this means that arbitrarily small concurrent reads (2 sectors or more) can observe torn writes if those writes are concurrent, but if the client is doing that they probably deserve it.

**request structure** - each async request has an associated `request` instance, created by a per-layer factories with arguments appropriate to the level. These are linked to each other with the following interface:
- `run(request *parent)` - submit a child request for execution
- `notify(request *child)` - invoked on completion of the child request
- `wait()` - wait for completion
- `release()` - release a completed request

Complicated (i.e. read) requests can keep a count of sub-requests which they decrement on each `notify`, and can use the argument to `notify` to release the sub-request.
Synchronous operations can be performed by launching a request and then waiting on it.

## Write cache

Note that both the read and write caches do everything in units of 4KB pages, giving a max cache size of 16TB. The translation layer itself handles arbitrary sector offsets.

The write cache is a circular log, with journal entries (see `journal.h`) that look like this:

|	journal entry	|
| :----------: |
| magic \# |
| sequence \# |
| length (pages) |
| checksum |
| metadata |
| ... |
| pad to 4KB |
| data  |
| ... |
| pad to 4KB |

The "metadata" field is a list of LBA,offset pairs indicating the logical address of the corresponding section of the journal record. (similar to the ext4 journal, except that it uses start/length encoding ("extents") instead of a per-block table.
The header takes a single 4KB page, and data starts on the next page.
Journal entries have consecutive sequence numbers, and we can verify that a complete entry was written by checking the CRC across the header and all the data pages.

If the next write would overflow we write a "pad" journal entry to fill up the remaining space and start at the beginning again.

We keep two extent maps in memory: a forward map from LBA to physical location (i.e. offset in the journal), and a reverse map from physical location to LBA.
The forward map is used for reads, and the reverse map for finding any remaining live data when the journal wraps around. 

Eviction is done by dropping the data, which means that the read cache only gets filled via backend reads.
This is because our first efforts to combine the two caches failed; we'll measure things, see whether it's a problem, and try again if it is.

The write cache periodically serializes its map and writes it to a reserved section of the cache file, alternating between two locations. When it's done it updates the write cache "superblock" with (a) the location of the latest "checkpoint", and (b) the location of the journal head when the checkpoint was created.

### consistency worries

Currently we update the translation map in the order in which NVME writes complete; however if we crash and recover we'll update the map in strict journal order, which may be different. 
If you have concurrent writes to overlapping locations which end up in different journal entries, and the journal writes complete out of order, this could result in a case where the runtime map diverges from the map calculated from the journal.
Either map would be a legal outcome of the I/Os, but it's **not** legal to change from one to the other after a failure.

Two possible solutions:
1. delay out-of-order completions, so that we update the map (and ack writes back to the client) strictly in journal order
2. record completion order in the log - e.g. add an array to the journal header giving sequence numbers for the last N completions

I kind of like (2) better, because it doesn't delay any completions to the user, but (1) is a lot simpler to implement so I'll do it instead.
In addition  (2) isn't totally fool-proof, as it doesn't preserve ordering of the last few journal write completions before a crash.

## Read Cache 

The read cache is indexed by object/offset rather than LBA - it uses the translation layer map to translate an LBA to an object/offset value, checks to see whether the corresponding section of the object is in cache, and requests it from the backend if not.

We do this for two reasons:
1. It avoids the "stale read problem", where (1) a backend read is issued, (2) an incoming write updates the same LBA, (3) the returning backend read updates the map to point to stale data.
2. I think it lets us share cache between volumes - if they're clones of the same base, then they can share cached contents of the same objects.

Orran and I strongly disagree about this design decision :-)

The cache is pretty traditional - it holds a fixed array of large blocks (64KB, subject to tuning), using the block size to essentially do prefetching.
Eviction is random, although I'm thinking of implementing CLOCK. 

*Random read protection* - with these parameters a 4KB random read will result in a 64KB read from the backend, plus a 64KB write to the SSD, with pretty bad performance. 
To prevent this, the read cache tracks the volume of data requested from the server and the volume of data returned to the client. If the ratio goes over 2:1 (subject to tuning) then reads are sent directly to the backend until the ratio is back down.

TODO: zero these out when the volume is idle, so that a long period of good cache performance doesn't set us up for a long period of thrashing if cacheability drops suddenly.

Note - for another project I've been spending a lot of time lately looking at the CloudPhysics traces, which are from real virtual machines that folks were paying Carl&Irfan&Co to manage, and the disk accesses from some of them are **really** not cachable.

## Object Translation Layer 

The translation layer is write-only, as all data fetching is done by the read cache.
(not quite - there's a debug read function)

At this moment [9/30/22] the code is quite messy - it hasn't been converted from closures to requests, GC hasn't been split out, and there's still some serialization code that should probably be pushed into `objects.cc`.
It basically copies writes into a batch buffer, then when the batch is big enough it updates the map and dispatches a write to the backend.

The rest of this section is mostly concerns about things that may need to be changed.

**write interface** - currently the write cache is responsible for passing writes to the translation layer.
I think this should be done from the high-level write request, after completing the request back to the client.

**thread pool** - currently the translation layer uses a thread pool, although I'm not sure how necessary that is now that backend writes are asynchronous.

**checkpoints** - currently checkpoints are done from a separate thread.
I think after writing a batch we should check whether it's time to write a checkpoint, and then do it.

**write coalescing** - the current code just concatenates all the writes in the order received.
We should get a significant boost in performance if we coalesce writes within a batch, as it reduces write traffic, reduces map fragmentation, and makes GC more efficient.
This won't be too hard, as most of the work can be done by a map variant in extent.h.
(it maps LBA->char\*, so you can enter the writes (in order) and their buffer locations, then read them out)

**locking** - the object map is guarded by a reader/writer lock.
Note that updates are fairly infrequent - a burst for every batch written to the backend for either data or GC.
If we order the updates by LBA (see "write coalescing") they should have very good cache behavior.

**memory usage** - it's possible that a highly fragmented map for a large volume will use lots of memory.
Memory usage is about 24 bytes per extent - 16 bytes with ~1.5x expansion - so a 1TB volume fragmented into 16KB extents would be 61M extents or about 1.5GB.
For typical use I think defragmentation will keep the map to a feasible size, however we may need to look at strategies to make sure sustained random writes don't mess things up.

**incremental checkpoints** - the periodic checkpoint process serializes the entire map to a single object.
For bigger instances this might be a problem: (1) it will be inefficient, since it re-writes unmodified entries, and (2) it will create huge objects.
I'm not sure how big a problem (2) is - it's not too much of an issue with S3, but I don't know about RADOS. (especially 3-replicated disk-based)

The solution is to do incremental checkpoints, keeping the last N - each contains any new entries since the last checkpoint, plus any remaining entries from the old checkpoint that just rolled out of the N-checkpoint window.
Support is already there: the extent map keeps D bits, and the object format allows specifying more than one checkpoint which must be loaded before rolling the log forward.

**[DONE] stalling** - for consistency the map needs to be updated in write order. This can be done after an object write completes, but this is complicated, especially in the case of GC writes. 
It's a lot easier to do the update when launching the write, but this raises the possibility of a read before the write is complete.
(see GC for discussion of a fix)

Solutions: (1) ignore it, since the write cache should prevent this from ever happening. (2) provide a mechanism to stall reads if there are writes outstanding to those addresses.
In the long run we probably need to do (2), since GC means that the data may not actually be in the write cache.

Stalling is implemented for both the write cache (i.e. don't overrun the circular buffer) and for the backend, which we declare busy after there are N object writes in flight.

## Cleaner (garbage collector) 

The garbage collector operates in very large passes:
1. Find the lowest-occupancy objects
2. Read any live data from them and store it in a local file
3. Iterate through the LBA space, doing the following:
    - grab old saved data and add to a batch
	- write the batch out when full

For each object written in step (3) we lock the map once, checking whether a bunch of regions we *think* need garbage collection really do, or whether they've been overwritten since we started the process, and assigning an object sequence number.
Then we assemble the GC object, write it to the backend, and continue.

This defragments the address space, avoids holding locks for too long, and seems to give good write amplification in simulation. It needs more testing and tuning, though.

**[DONE] read/GC interference** - if we update the object map when we start writing out a GC object, there's a chance that an incoming read will come in for data that's getting copied.
There are a few ways to deal with this:
1. deferred map updates. Really hard, because other write batches are going to follow the GC write, and we have to keep proper ordering
2. redirect reads to the in-memory copy. Hard for accidental reasons - it crosses boundaries of the abstractions the code has been split into.
3. stall reads to data that's being garbage collected

I think (3) is the most reasonable choice - keep a map (in the top-level object) of blocked LBA ranges, and a condition variable to wait until the read isn't interfering with GC anymore.
The map will be smaller and simpler if we just block out the entire LBA range that's currently being cleaned.

Actually, all we need to do is wait until all objects up to the one being read have been persisted to the backend. It works both for GC and for anything that gets missed in the write cache.

**cleaner/translation split** - I haven't started this yet, and I'm starting to think that it might be a bad idea.
In particular we need to keep track of when object writes complete, because (I think) we need to know when all objects up to sequence N have been successfully written, but GC, checkpoints, and data all share the same sequence number space.
I think it makes sense to have a single object instance, but maybe split the method implementations into multiple files.

## To do

**[DONE] async requests** - The read cache, translation layer, and some debug functions still use a closure mechanism that's in the process of being replaced by request instances.

**[DONE] in-memory objects** - the translation layer has a mechanism for accessing recent objects which haven't been fully written to the backend, which needs to be removed. It dates to before the write cache was implemented, and it's of no use anymore.

**Image create/delete** - this is currently handled outside of the library by some of the test Python code; obviously it needs to be moved into the library itself

**[DONE] Config file** - it's reached the point where it needs a simple config file reader, which will be throw-away code since I assume we'll use Ceph config when upstreamed.

**[SKIPPED] Split translation, cleaning** - GC is currently part of the translation layer module.
I think it would be best to factor this out, as it's a significant piece of complexity in its own right, however the two are going to need to share some common functions for handling sequence numbers and completions, since the objects they write are ordered by a single sequence of sequence numbers.

Turns out it may not have been such a bad idea after all.

**Snapshot, clone** - these need to be implemented and tested

`io_uring` - I had delayed switching to `io_uring` because I had no idea when it would show up in RHEL, but now that it's there I don't think I can justify using `libaio`

[whoops - RHEL 9 has 5.14, but I think they disabled `io_uring`]

## Random notes

remove uuid from all the journal entries except the header
