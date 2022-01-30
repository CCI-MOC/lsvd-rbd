# new-lsvd

thread pool:

took a stab at using https://github.com/DeveloperPaul123/thread-pool,
which seems to be a nice header-only thread pool, but it's a pain in
the ass to compile.

Instead think I'll use:
https://github.com/fbastos1/thread_pool_cpp17

from
https://codereview.stackexchange.com/questions/221626/c17-thread-pool

# basic design

Need to implement lots of the functions from [librbd.h](https://github.com/ceph/ceph/blob/master/src/include/rbd/librbd.h)

This seems to be a list of most of the methods that we should implement - note that in a lot of cases we probably have to figure out which version (e.g. create/create2/create3/...) the tools are using

- `rbd_list2` - list images
- `rbd_create`, `rbd_create2`, `rbd_create3`, `rbd_create4` - various ways of creating image, we should pick one?
- `rbd_clone`, `rbd_clone2`, `rbd_clone3` - create a clone; again, pick 1?
- `rbd_remove`
- `rbd_open`, `rbd_open_by_id`, `rbd_aio_open`, `rbd_aio_open_by_id`
- `rbd_close`, `rbd_aio_close`
- `rbd_get_size` - [maybe]
- `rbd_copy`, `rbd_copy2` .. 4 - [maybe]
- `rbd_snap_list`, `rbd_snap_list_end`, `rbd_snap_exists`
- `rbd_snap_create`, `rbd_snap_create2`
- `rbd_snap_remove`, `rbd_snap_remove2`, `rbd_snap_remove_by_id`
- `rbd_snap_rollback`
- `rbd_list_children` - not sure we can do this (list children of clones)
- `rbd_read`, `rbd_read2`, `rbd_read_iterate`, `rbd_read_iterate2`
- `rbd_write`, `rbd_write2`
- `rbd_discard`, `rbd_aio_discard`
- `rbd_aio_write`, `rbd_aio_write2`, `rbd_aio_writev`
- `rbd_aio_read`, `rbd_aio_read2`, `rbd_aio_readv`
- `rbd_aio_create_completion`, `rbd_aio_is_complete`, `rbd_aio_wait_for_complete`, `rbd_aio_get_return_value`, `rbd_aio_release`
- `rbd_flush`, `rbd_aio_flush`


### Phases of development

1. basic read/write functionality, with working garbage collection. Image creation via external utility

2. logging of journal map, local journal recovery

3. incremental checkpointing of backend map, failure recovery.

4. clone - read/write support in library, creation via external utility

5. snapshot - creation by external utility while volume is quiesced, add deferred GC delete to library

6. fancy GC / defragmentation

7. roll snap/clone external support into the library??? not sure how important this is

### Journal and local cache format

Need to specify detailed format of the journal:

- individual records
- allocation (bitmap?)
- how do incoming writes and cached reads go together?
- eviction algorithm
- map persistence
- crash recovery process

Note that we can do 1-ahead allocation so that we can link journal records both forwards and backwards.

Hmm, we can allocate in fairly big chunks (makes the journal header a bit more complex) which means that the bitmap can be smaller. E.g. 4KB = 32MB bitmap for 1TB, but 64KB = 2MB bitmap.

But that's also problematic, since it means we may need to do GC to free blocks, so maybe we're stuck with 32MB/TB.

### Object format / header

This needs to handle a bunch of stuff for recovery, snap&clone, incremental map persistence, etc.

I've got some notes somewhere about it.

### Software structure

Each virtual disk instance has:

- LBA -> (obj,offset) map, for all complete write batches
- LBA -> ptr map for any writes currently being batched
- (obj,offset) -> LBA map to find cached data
- current write batch (process when we hit size or timeout limit)
- outstanding writes to backend
- outstanding reads to backend

I've been thinking that there's a single write batch buffered in memory. That means when it fills we need to atomically:
- sort and merge it
- assign an object number, put extents into LBA->(obj,offset) map
- wait for NVMe writes to complete
- set new empty buffer, map

We need NVMe writes to complete, since the only way here to access new data after it's no longer in the newest buffer is to get it from the NVMe.

Actually I think it would be better to have a list of buffer/map pairs, and we can remove a pair from the list and recycle them when all their NVMe writes complete.

We don't need to worry about new writes after that point, since they should last in the journaled cache until after they've been successfully written to back-end S3.

So the order of checking things for a read is:
- incoming buffer+map (LBA->ptr)
- list of buffer+map with NVMe writes pending (LBA->ptr)
- backend map (LBA->(obj,offset))
    - now check cache map (obj,offset)->LBA
        if found, read from cache; otherwise fetch remote

Incoming buffer+map needs a reader/writer lock, as it's shared between reads and writes. The buffer+map pairs waiting on NVMe write are read-only, but we need a reader/writer lock on the list so that they don't get deleted while we're looking at them.

The backend map and cache maps can both have single reader/writer locks, writes should update cache map before backend map, one at a time, for consistency.

### "Instantaneous garbage collection"

1. calculate all the data to garbage collection
2. read it in, save a copy in NVMe
3. atomically update it in the backend map and the cache map
4. write it to the back end

Step 3 will lock the backend map for a while, since we have to re-check all the calculations from steps 1 and 2, but it's still in-memory. If it's all written to NVMe cache, then step 4 can happen **after** the map update and we're still OK.

Note that the data written to NVMe might be quite cold, so once the writeback in step 4 is finished we may want to prioritize it for eviction.

### Cache replacement

The extent map implements A and D bits; D is set implicitly by update operations, while A is set explicitly by the application.

This allows us to do rolling checkpoints for the object map, as long as we don't support discard. Discard is tricker - we could probably implement it by mapping to a sentinel value, e.g. object = MAX, offset = LBA. Not sure if we should ever remove these from the map - with 24 bits for the length field, max extent size = 2GB so it won't add many extra extents. 

A single A  bit restricts us in the replacement algorithms we can use. One possibility is 2Q/CLOCK, maintaining 2 maps:

- CLOCK on first map moves cold extents to 2nd map
- CLOCK on 2nd map chooses extents for eviction
- update trims from 2nd map and inserts into first map
- lookup has to check both maps and merge the results. 

### lazy GC writes

Data objects need to have pointer to last "real" data object, so we can ignore missing GC writes

### object headers

Standard header
```
magic
version
volume UUID
object type: superblock / data / checkpoint
header len (sectors?)
data len (sectors?)
```

Superblock
```
<standard header>
volume size
number of objects?
total storage?
checkpoints: list of obj#
clone info - list of {name, UUID, seq#}
snapshots - list of {UUID, seq#}
pad to 4KB ???
```
Do we want a list of all the outstanding checkpoints? or just the last one?

Data object
```
<standard header>
active checkpoints: list of seq#
last real data object (see GC comment)
objects cleaned: list of {seq#, deleted?)
extents: list of {LBA, len, ?offset?}
pad to sector or 4KB
<data>
```

Checkpoint
```
<standard header>
active checkpoints: list of seq#
map: list of {LBA, len, seq#, offset}
objects: list of {seq#, data_sectors, live_sectors}
deferred deletes: list of {seq#, delete time}
```

### random thoughts

GC - fetch live data object-at-a-time and store it in local SSD. Note that we have to flag those locations as being temporary allocations, so they don't get marked as "allocated" in a checkpoint.

If we allocate blocks of size X (8KB, 16KB etc) then we free blocks of that size. With X>=8KB we don't need to worry about bookkeeping for headers. Freeing is complicated, because we have to check that the adjacent map entry isn't still pointing to part of the block, but at least we don't need to keep counters for each block. For now stick with 4KB, as it's probably small compared to the extent map.

(hmm, if we allocated 64KB blocks and kept 4-bit counters of the number of live 4K blocks in each 64KB, we could reduce the bitmap size by 4. Then again, just going to 16KB would do that, and we could still use bitmap ops and other good stuff)

1TB / 16KB mean extent size would be 128M extents, or 2GB RAM. Note that we can probably structure the code to merge a lot of writes going to NVMe - post the write to a list, each worker thread grabs all the writes currently on the list.

objects.cc - need an object that takes a memory buffer and can return pointers to header, other parts, and iterators for the various structures.
It should also be able to serialize a set of vectors etc. Probably need std::string versions of the data structures that have names in them.

Need a convention for how to handle buckets. I think including them in the name - "bucket/key" - is fine. Remember that this is going to also have to work for files and RADOS.

How do we structure the local storage?

superblock has
- timestamp
- remote volume UUID
- pointer to last chunk written
- <bitmap info>
- <map info>

note that temporary allocations are going to need some sort of journal header. Actually, can just flag them as such, and they get freed on replay.

Bitmap and map are stored in the same way - a count (of bits or map entries) and a list of block numbers containing the data. (if we use 4KB block numbers we can use 32 bits for everything and still handle 16TB local SSDs)

actual format in the superblock is probably
- int64 count
- int32 block count
- int32 data block 
- int32 indirect block 
- int32 double indirect block
- int32 triple indirect

Use convention that only one of data / indirect / double indirect are used. 

- data block: 4KB of data
- indirect: 1K 4KB blocks
- double: 1M 4KB blocks = 4GB
- triple: 4TB, i.e. more than enough

So maybe:

- int32 count
- int32 tree level (1, 2, 3 or 4 for direct / single..triple indirect)
- int32 block count
- int32 block

Allocate storage and write all the data for bitmap and map before writing the superblock. Keep 2 copies of the superblock, write the first synchronously before writing the 2nd. On restart take the one with the most recent timestamp.

Block allocator needs to return the block number for the next one that will be allocated, so we can do forward linking for log replay.

SEQUENCE NUMBER. That's what the timestamp is - it's a counter that's incremented with every write. We need that in the journal header.

Keep the CRC in the journal header, note that we only have to check it on log replay.

## Possible problems

`int64_t` vs `uint64_t` - we're losing a bit of precision in a bunch of places because of this. I think we need to move to unsigned integers for the bitfields, but I'm worried about signed/unsigned problems.

- **DONE** changed to unsigned. In particular, signed bitfields of length 1 don't work very well...

extent length - what happens if we try to merge two adjacent extents that overflow the length field?
TODO - need to check for this; maybe modify the `adjacent` test so it fails on overflow.

extent length -  what happens if we try to insert a range that's too large? This should only happen if we're implementing discard with a tombstone sentinel, as we're not going to create objects large enough or allocate memory or NVMe space large enough. In that case we have to loop and insert multiple.

## Schedule

1. finish extent map, unit tests
1. basic non-GC device, using librbd? BDUS?. Need simple "mkdisk" utility. File backend?
1. superblock 
1. startup / recovery code
1. simple (full) checkpoints
1. simple GC
1. incremental checkpoints
1. clone
1. snapshots
1. backends: S3 / file / RADOS
1. better GC

How to set up unit/integration tests?
- read tests with prefab data
- write tests
- python tools for generating / parsing object data
- python driver script? Or use libcheck?
