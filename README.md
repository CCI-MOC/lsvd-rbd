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

