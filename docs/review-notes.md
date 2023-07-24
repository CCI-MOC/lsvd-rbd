Notes for Friday code review, i.e. a quick guide to the code.

`fake_rbd.h` - this is the portion of the RADOS Block Device API that we support. Basic functions are:

- create image, delete image
- open image
- read/write:
	- byte offset, length (sector-aligned)
	- aio or synchronous
	- buf/len or iovec

`request.h` - I/O request structure that gets used through almost all the remaining stuff.

- typically created by a layer-specific function, e.g. `backend->make_read_req(...)`
- synchronous:
```
		r = make_req();
		r->run(NULL);
		r->wait()
```
- async:
```
        r->run(request r2);
```
        - on completion `r2->notify()` is invoked.

`lsvd.cc` - implements the RBD functions. The main data structures and types are:

- `rbd_image` - defined in `image.h` because `lsvd_debug.cc` uses it too. Holds all the parts:
	- config from file/environment vars
	- translation layer
	- write cache
	- read cache
	- object map (shared between translation layer and read cache)
- `lsvd_completion` - implements RBD API completion semantics
- `rbd_aio_req` - state machine for LSVD read/write requests

The code was originally written using continuation passing, with per-request state in local variables. The old organization hasn't been fully fixed - `lsvd_completion` and `rbd_aio_req` should be merged, and the aio request state machine is pretty inelegant.

Asynchronous reads are complicated, because sections of a single read can (a) hit in the write cache, (b) hit in the read cache / translation layer (misses in the read cache aren't visible here), or (c) be entirely unmapped so we return zeros. The read interfaces to the write cache and read cache look like:

- `auto [skip, read, req] = read(start, len);`
    - start through start+skip not found here
    - start+skip through start+skip+read was found
    - req is the async request for the section found (or null)

So you iterate through requested address range, taking the regions that get skipped by the write cache and passing them to the read cache, clearing any regions that get skipped by the read cache, and accumulating a list of requests that all have to complete before the user request is done. It's kind of gross, but seems to work pretty well.

`objects.h`, `journal.h` - these are the data structures used in the backend objects and in the write/read cache. 

Objects have 3 types: superblock, data, checkpoint, and consist of a header and (data only) a data section. The checkpoint has (a) the map and (b) a list of all objects and sizes, live data etc. The data object header indicates the LBAs for all the data. The object sequence number is encoded into its name - e.g. `image_name.00000f05`. All addresses and lengths in the objects (except e.g. type/length encoded fields) are in units of 512-byte sectors.

Note that backend data is pretty universally identified by object number and offset (in sectors) within that object. For now converting from object number to object name is basically `sprintf("%s.%08x")`, but it will be a tiny bit more complex when clones are implemented.

There is a single cache file, because it was originally intended to be a partition, divided into read and write cache sections. Almost all stuff in the cache is in units of 4KB pages, except that LBAs are in sectors.

Write cache: circular buffer, kind of like ext4 journal. The journal entry header includes LBA and sequence information, followed by data pages. On clean shutdown the write cache map is saved for restart; when starting with a dirty cache we just read the entire journal.

Read cache: array of (currently) 64KB blocks from backend objects, plus an array where we save the address of the current contents of each block. My tests are failing after a few dozen crash/restart cycles because the map and the block contents are getting out of sync.

`extent.h` - implements an extent map that gets used for both the caches and the backend. Supports a few different map types:

- LBA to (object# / offset)
- (object# / offset) to LBA - not used at present
- LBA to char ptr
- LBA to LBA

It's not general-purpose, and does some things with bitfields that make strong assumptions about how big a disk can be, etc.
It includes a few features useful for a translation layer - in particular  update can return a list of the old extents that got over-written. It's been tested long enough that I'm fairly confident in it, and haven't found any bugs while writing the LSVD code.

`backend.h`, `rados_backend.cc`, `file_backend.cc` - uniform interface to backend object storage. The file backend is for debug / testing and has some stuff related to that, and isn't written for performance. S3 hasn't been written yet.

`nvme.h`, `nvme.cc`, `io.h`, `io.cc` - libaio-based interface to SSD used by the read and write caches, using request structures. Not sure why it's factored into 2 files. It should be straightforward to switch this to `io_uring` - it's libaio because Red Hat doesn't support `io_uring` in RHEL.

`write_cache.h`, `write_cache.cc` - what it says.

backpressure - before sending data to the write cache you need to:

- call `write_cache->get_room(sectors)` - there's a max amount of outstanding write data
- call `translate->wait_for_room()` to wait until the number of outstanding backend object writes is within its window
- send your async write to the write cache
- call `write_cache->release_room(sectors)` when it completes.

There are a whole bunch of debug methods and functions that are only used for the old unit-testing code, which used Python ctypes to interface to functions in `lsvd_debug.cc` - in particular they expose a lot of functionality at the level of the write cache, read cache and translation layer. Not sure this is needed anymore.

`config.h`, `config.cc` - really simple config-file reader. Config file variables have the same names as corresponding fields in `class lsvd_config`, and you can set them with an environment variable with the upper-cased name prefixed with `LSVD_`

`test7.cc`, `test8.cc`, `test10.cc` - Test 7 repeats a process of doing random writes of random data and reading them back. Test 8 does that from multiple threads. Test 10 writes in a subprocess, which exits with outstanding writes in progress, opens the image in the parent process to read it back, and then repeats. In each case we verify the CRC32 of the data fetched from each sector. (plus it's stamped with LBA and write sequence number at the beginning of each sector, which gets used by some of the test code and is really useful in debugging)

Garbage collection is kind of crude:

- make a list of objects to clean
- fetch each object and write into a great big file
- make a list (in LBA order) of all the LBA extents pointing to to-be-cleaned objects
- while list not empty:
	- take enough entries to fill an object
	- validate them against the current map
	- read the data from the local file
	- write it out

To handle some of the consistency issues we always update the object map *before* writing the objects out, rather than after the write completes. We assume that the write cache is big enough that we never get requests for objects being written. This actually isn't the case during garbage collection, so the read cache has to call `translate->wait_object_ready(obj)` before sending a request to an object.

Updating the map before writing makes it easier to:

- update the map in a consistent order, i.e. the same order that will be used for post-crash roll-forward
- avoid locking over I/O operations in GC - if you grab a sequence number and an object worth of LBA to object/offset mappings, you can drop the lock, retrieve them from the local file, and write it out. Any following writes will go into higher-numbered objects, and so will override the GC object.

We rely on maintaining a single write order in the write cache and object stream, which sometimes requires a bit of re-ordering, as NVMe writes might complete in an order different than we would replay them during crash recovery. There are vastly different mechanisms for re-ordering completions in the write cache and the translation layer; I don't know why they're different, and can't remember if we even need to re-establish order in the translation layer. (except maybe for flush)

Write cache recovery - each journal header has a "previous" pointer, so we can look backwards from the start of the circular buffer to find the beginning, then roll foward until the end. To bring the backend up to date we replay any writes that aren't represented in the backend - each object includes the write cache sequence number for the first write it holds. (that means the replay duplicates one object worth of data, which isn't a big deal) For real use it might be OK just to replay the entire contents of the write cache, but being more selective made some of the tests easier.

Read cache recovery - the read cache is really simple. It uses random eviction, and evicts in batches to amortize the cost of re-writing the cache map after eviction. Map persistence logic:

- blocks don't go on the freelist and get zeroed in the map until after the map is written
- they retain their contents until after they are allocated from the freelist
- the in-memory map isn't updated to point to a block until it has been written
- the map is written periodically (currently 2s) if it has changed

This **ought** to prevent the map from getting out of sync, but it doesn't. I assume I did something stupid.

The write cache aggregates multiple higher-level writes into a single NVMe journal entry. It won't batch if there are no outstanding requests, so sync I/O shouldn't be impeded, and a partial batch will be written after a 50ms idle timer (hmm, should make this configurable). The translation layer flushes a batch to the backend when it hits a configured size (currently 8MB), or when a configurable idle timer is hit. The idle timer fires if (a) the current batch is non-empty, and (b) it has been more than T seconds since the last object was written (default 2s), so data loss from a crash with cache failure is limited. 

For the most part I've stamped out all the gratuitous uses of threads that were in some earlier versions. There are threads for stuff like flushing and garbage collection, and a bunch in the file backend, but all the read/write logic is performed either (a) directly from the original call to an aio request, or (b) from a completion callback of one of its stages.
