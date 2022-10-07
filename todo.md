# punch list

**GET FIO WORKING**
- read seems to have regressed totally
- write seems to hang on completion when compiled with -O3
[DONE]

## config file
[DONE, needs to be integrated]
things to go here:
- write batch size
- write window outstanding
- directory for cache files
- number of write cache threads
- which backend to use?

Can probably have sections that override settings on a per-volume basis

## cache file handling

**split read/write** - if we're going to use files, there's no reason why the two caches can't go in different files.

[DONE] **naming** - default name is volume UUID, or "UUID.rcache", "UUID.wcache"

[DONE] **creation** - volume startup should be able to create cache if none exists
note that we still don't handle malformed cache files

## other stuff

**write cache sequence number** - need to record this in the backend so that we can implement write cache roll forward properly.

**read blocking** - need to block interfering reads during GC 

**garbage collection** - need to make it work properly, then test it

**write pacing** - implement pacing for the backend.

Note - I had been thinking about having the RBD level (`lsvd.cc`) pass data to the translation layer after write cache completion, but this won't work, as it won't preserve the write ordering in the cache. It will result in a *legal* ordering, but if the backend and cache differ, volume could change after crash recovery.

**top-level structure** - [DONE] `fake_rbd_image` was just an
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
- is there any use for `req->wait` method?

`rados_backend.cc`:
- should `rados_backend` take an ioctx rather than using pool in the prefix?
- handle multiple pools in rados backend
- [DONE] shut down RADOS state on rados backend delete

`lsvd.cc`:
- implement `rbd_aio_discard`
- implement `rbd_aio_flush`
- implement `rbd_aio_readv`, `rbd_aio_writev`
- `rbd_aio_req` - clean up the state machine, merge it with lsvd_completion

`read_cache.cc`:
- implement CLOCK replacement
- lots of notes that documentation needs to be added

`translate.cc`:
- get rid of global UUID
- [DONE] initialize `last_ckpt` - done? need to check
- `translate_impl::worker_thread` - coalesce writes
- GC in general, also something about the objmap lock...

`write_cache.cc`:
- it looks like we might not be freeing `wcache_write_req` properly?
- switch metadata regions when writing checkpoint
- something about `super_copy->next`
- `roll_log_forward` - NOT IMPLEMENTED 
