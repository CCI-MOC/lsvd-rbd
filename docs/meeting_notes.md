## W7-4

- Kristi 
	- get CI working, try to build a single nvmeof gateway artefact
	- move clone.py back into translate.cc
- Sumatra 
	- readahead in read cache
- Isaac
	- image striping
- Timothy

## Todos W6-1

Current todos:

- Refactor top-lv requests into lsvd class
- Image striping
	- Make sure ordering is preserved between stripes
- SPDK fork for new LSVD backend
- Potential switch of backend to RBD instead of s3/rados
- General production ready testing

Question: how to prioritise the above? who works on what?

## Todos W49-2

- RDB and LSVD on both HDD and SSDs, all 4 configurations
- Working set (80g) < nvme cache (500g)
- Fio synthetic workloads, vary blocksize, io depth, workload
- Filebench (varmail, oltp, fileserver, fileserver-fsync?), maybe YCSB
- Shared cache VM boot times

Notes on write log:

- Remote malloc
- Remote nvme
- 2 remote nvme
- Measure all of this with a single write workload, reads don't matter here

## Todos W49-1

- By W49-2, run benchmarks for:
	- SSD backend
	- HDD backend
	- blocksize 4K, 8K, 16K
- By w49-3, set up VM boot for shared cache

Notes on VMWare proposal:

- IOPs guarantee for certain users

## W48-5: to get done week Dec 4-7 for presentation dec 12th

- filesystem benchamrks - fix OLTP - this weekend - same parametser as eurosys Logical disk and cache both
- working set for disk benchmark < size disk - same as eurosys
- all above with Ceph disk backend - run both filesystem and disk bencharks - this will require much larger read cache block size
- trace-driven optimization - from start to adding a print statement in init - VM boot
- shared cache - boot multiple clones demnstrate: 1) easest - boot clone after ahving booked another one becomes warm show both miss rate and boot time, 2) boot time for a fleet of similar images 10 VMs simultaneously
- test larger block sizes for read cache and impact on trace optimization - i.e., prefetching - especially for disk backend
- presentation drafts by the 6th

### stretch goals
- write-ahead log should be memory-only - want to show it with 1- malloc store, 2- versus with 1 NVME log, and 3- 2 NVME log
- NVME rather than ramdisk
- iscsi target 

## ATC 24 todos
- benchmark against nvme drive instead of ramdisk
- trace-driven optimisation of VM boot image - timo
- ycsb benchmark?
- iscsi target, effort should be minimal
	- re-attach to the same local cache
- make sure shared cache and cloned images work
- per-commit benchmarks - sumatra
- write-ahead log should be memory-only instead of going to nvme
	- with UPS, failure mode we address is failure of gateway server

Paper thesis:

- storage gateway that's fast, scales, deploys as nvmf target
- GC performs well (?)
- shared gateway, read sharing allows for better performance?
	- shared cache
	- shared images between VMs
	- physical machines with hardware we don't have (NVMF)
- shared gateway and cloned images
	- since they're derivations, they share prefixes
	- thus share cache
	- fleet deployments -- most machines are mostly the same and derived
	- faster startup due to warmed cache from other machines booting?
	- container-like sharing of base images, potential performance advantage
- disaggregation of backend

2023-12-12 talk with vincent from IBM:

Hey guys, I think we should have a series of 20 minute presentations.  This is a
good deadline 12/12 to have results on the various efforts.  Perhaps Peter and I
giving an overview of the group vision, the larger opportunity that pulls
together LSVD and D4N, how this ties into data center architecture/MOC/AI, and
then a series of targeted talks on:

- Performance of LSVD for block and file system benchmarks
- LSVD re-organizing images for fast boot
- LSVD sharing of cache state
- D4N implementation and initial performance results
- D4N locality integrated into k8s


## old todo list

`lsvd.cc`:
- implement `rbd_aio_discard`
- implement `rbd_aio_flush`
- `rbd_aio_req` - merge with lsvd_completion 
- `rbd_remove` - find and remove cache file

`read_cache.cc`:
- implement CLOCK replacement
- lots of notes that documentation needs to be added
- **code cleanup** - stop using saved superblock for runtime variables 

`translate.cc`:
- coalesce writes
- improved GC

`write_cache.cc`:
- **batching** - account for write size (not just number of writes) in batching
- **code cleanup** - stop using saved superblock for runtime variables 

**unused fields in superblock** - `total_sectors`, `live_sectors`, `next_object` - either use these or delete them.

**putting data into read cache** - do this from translate when it's  writing a batch. Data will get written twice, but there won't be a read operation. Make the write cache a fixed size, dependent on backend write window and batch size, and read cache takes all the rest of the space

**improved GC** - interface to get data from read cache, also decide whether to do partial or full read of an object.

**clone volumes**
- create script for clone. note that we

**snapshots**

# all old stuff below here

[DONE]**GET FIO WORKING**
- read seems to have regressed totally
- write seems to hang on completion when compiled with -O3

## config file
[DONE]
things to go here:
- write batch size
- write window outstanding
- directory for cache files
- number of write cache threads
- which backend to use?

Can probably have sections that override settings on a per-volume basis


## cache file handling

[NO] **split read/write** - if we're going to use files, there's no reason why the two caches can't go in different files.
(yes there is - it makes it harder to use a partition)

[DONE] **naming** - default name is volume UUID, or "UUID.rcache", "UUID.wcache"

[DONE] **creation** - volume startup should be able to create cache if none exists
note that we still don't handle malformed cache files

## other stuff

[DONE] **write cache sequence number** - need to record this in the backend so that we can implement write cache roll forward properly.

[DONE] **read blocking** - need to block interfering reads during GC. (note - this can be done just by blocking access to any objects which haven't finished being written out, and this works for misses in the write cache, too)

[DONE] **garbage collection** - need to make it work properly, then test it

[DONE] **write pacing** - implement pacing for the backend.

Note - I had been thinking about having the RBD level (`lsvd.cc`) pass data to the translation layer after write cache completion, but this won't work, as it won't preserve the write ordering in the cache. It will result in a *legal* ordering, but if the backend and cache differ, volume could change after crash recovery.

[DONE] **top-level structure** - `fake_rbd_image` was just an
afterthought. Need to re-architect it properly.

**merged caches** - Is there any way we can move stuff from the write cache to the read cache? 

**race conditions** - scrub notification methods to look for race conditions like the read cache one.

[DONE] Checkpoint list weirdness:
```
    ckpts:     68 : 4294967295
```

translate threads - do we actually need multiple threads, since we're
using async calls? probably not.

any other parameters that should go in the config file?

## list of TODO comments in code

`io.cc`:
- [DONE] `e_iocb` instance is self-deleting, fix this

requests in general:
- `request->run` - should this return success/error?
- [YES] is there any use for `req->wait` method?

`rados_backend.cc`:
- should `rados_backend` take an ioctx rather than using pool in the prefix?
- conversely - handle multiple pools in rados backend
- [DONE] shut down RADOS state on rados backend delete

`lsvd.cc`:
- implement `rbd_aio_discard`
- implement `rbd_aio_flush`
- implement `rbd_aio_readv`, `rbd_aio_writev`
- [DONE] `rbd_aio_req` - clean up the state machine
- `rbd_aio_req` - merge with lsvd_completion 

`read_cache.cc`:
- implement CLOCK replacement
- lots of notes that documentation needs to be added

`translate.cc`:
- [DONE] get rid of global UUID
- [DONE] initialize `last_ckpt` - done? need to check
- `translate_impl::worker_thread` - coalesce writes
- GC in general, also something about the objmap lock...

`write_cache.cc`:
- [DONE] it looks like we might not be freeing `wcache_write_req` properly?
- [DONE] switch metadata regions when writing checkpoint
- something about `super_copy->next`
- [DONE] `roll_log_forward` 
- [DONE] **write throttling** - need to (a) bound number of outstanding NVMe
  writes, (b) avoid "hanging" writes due to batching
- [DONE] **shutdown** - flush all writes and checkpoint before closing
- **batching** - account for write size (not just number of writes) in batching
- **code cleanup** - stop using saved superblock for runtime variables

## performance

`io_uring` - should shift from libaio to `io_uring` - currently taking
as much CPU for `io_submit` as for the rest of LSVD. Or can I just get
libaio to work correctly? Currently 5% of CPU (out of total 38.5% used
by LSVD) is going to `usleep`.

**locking** - can we use shared lock for `get_room`? any other locking
fixes to get rid of overhead?

## write cache CRC

should I do it? code is:
```
	#include <zlib.h>
	uint32_t crc2 = ~crc32(-1, (unsigned char*)h, 4096);

```
check the CRC by saving a copy, zeroing it out again, and recomputing.

Standard Linux zlib CRC32 isn't all that fast - 750MB/s on the old E5-2660 v2 machines, 2.1GB/s on the new Ryzen. Cloudflare zlib (https://github.com/cloudflare/zlib) is **way** faster - 21GB/s on the Ryzen and 1.9GB/s on the old HP machines.

## write recovery 

Right now write cache log recovery is a mess. Ways to fix it:

[DONE] **clean shutdown** - add a clean shutdown flag; if it's set, we read the lengths and map from the metadata section and we're done.

[DONE] **brute force** - on startup seach the entire cache to find the beginning of the journal, then roll it forward, updating the map and sending all the data to the backend.

**translation layer assist** - translation layer tracks sequence numbers (in write cache) of each write, and provides an interface to get the highest sequence number s.t. all writes before that have committed to the backend. Log replay is:
- read all headers to find start of log
- read headers starting at start of log to update cache map
- for any newer than recorded max confirmed (minus one), send to the backend again.

filtering out spurious journal entries with brute force - basically we go through the cache looking for blocks where the magic number is ok, and the starting sequence number is the lowest one we encounter.

But... there might be a spurious one. We can handle this by looking at it as the search for the start of the sequence after the gap, starting with the oldest sequence number. If the we find a block *b* with sequence number lower than all seen so far, it's a tentative start to the log. Check *b+len* etc. all the way to the end of the log, checking magic number and consecutive sequence numbers the whole way. If we stop partway, then this was a false start - throw away the sequence number information and start scanning for the magic number at *b+1*.

Note that it can only stop before the end if it begins at *b=0*.

**current status (10/21)** - has dirty/clean flag, checkpoints only on clean shutdown, does brute force to recover cache state and write it all to backend.

Remaining crash recovery optimizations:
- periodic checkpointing when using large caches so we don't need to go through entire cache. (is this necessary?)
- translation layer assist to avoid replaying entire cache to backend on crash recovery

## translation layer startup issues

after several crash/restart cycles, getting assertion failure due to object overwrite

## gc ideas

add a GC generation field to objects

