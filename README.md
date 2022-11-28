# Log Structured Virtual Disk

Described in the paper *Beating the I/O Bottleneck: A Case for Log-Structured Virtual Disks*, by Mohammad Hajkazemi, Vojtech Aschenbrenner, Mania Abdi, Emine Ugur Kaynar, Amin Mossayebzadeh, Orran Krieger, and Peter Desnoyers; EuroSys 2022

Uses two components: a local SSD partition (or file) and a remote backend store. (RADOS or S3)

The debug version uses files in a directory rather than an object store.

## status
As of 11/28, there seem to be two remaining bugs:
- occasional read hang, observed 2x on test10 and 1x on QEMU
- something to do with the read cache - occasionally returns data from the wrong object at the same offset (mod 128 sector block size) as the target sector. Reliably reproducible with test10
- QEMU regression - abort in malloc/free in batch::~batch
- QEMU regression - seems related to the hanging reads

## usage

Pick a RADOS pool, an image name, and a directory on an SSD-based file system (`rbd`, `img1`, and `/mnt/nvme/lsvd` in the example below).

1. create a disk - this creates a 4KB RADOS object named `img1` with the volume metadata:

    `sudo python3 mkdisk.py --rados --uuid 7cf1fca0-a182-11ec-8abf-37d9345adf43 --size 10g rbd/img1`

2. initialize the cache, using the same UUID. 

    `sudo python3 mkcache.py --size 400M --uuid 7cf1fca0-a182-11ec-8abf-37d9345adf43 /mnt/nvme/lsvd/img1.cache`

Note that the cache needs to be at least this big because the write cache is only 1/2 of it, and if it's too small (a) really large writes can deadlock it, and (b) it might not be able to hold all the outstanding writes to the backend, risking data loss if you crash and restart.

3. start KVM using the virtual disk. (note - you can use the file `lsvd.conf` instead of environment variables - see config.h / config.cc for details. Keywords are the same as field names.

    `sudo env LSVD_CACHE_DIR=/mnt/nvme/lsvd LD_PRELOAD=$PWD/liblsvd.so qemu-system-x86_64 -m 1024 -cdrom whatever.iso -blockdev '{"driver":"rbd","pool":"rbd","image":"rbd/img1","server":[{"host":"10.1.0.4","port":"6789"}],"node-name":"libvirt-2-storage","auto-read-only":true,"discard":"unmap"}' -device virtio-blk,drive=libvirt-2-storage -k en-us -machine accel=kvm -smp 2 -m 1024`

Notes:
- cache gets divided 1/2 read cache, 1/2 write cache
- LSVD looks in `$LSVD_CACHE_DIR` (or `cache_dir` from `lsvd.conf`) for (a) `<img>.cache`, then `<uuid>.cache`. If it doesn't find either it creates a cache with default parameters, but that's currently broken.
- you can omit the UUID, but then you'll want to have LSVD create the cache file so that it matches the remote image. In that case you'll need to set LSVD_CACHE_SIZE=400m or you'll deadlock eventually.
- also you'll want to set XAUTHORITY=$HOME/.Xauthority

Unit tests included here:
- test1.py through test5.py - really simple functional tests
- test6.py - replays a trace from the Ubuntu 14 install
- test7.cc - random writes and reads followed by readback, with CRC verification
- test8.cc - random writes from multiple threads, CRC verification
- test9.cc - similar, but crash and restart

Writes are done with a size distribution vaguely based on the test6 QEMU trace - mostly small, with a few up to 16MB.

## Overview

Each virtual disk has two sets of storage - a local SSD partition and a stream of objects on the backend.

### Backend objects

The objects are:
1. a "superblock", e.g. `obj`, holding basic volume information
2. a stream of numbered objects, e.g. `obj.00000001` etc.

for the different backends,
- File: specify a directory and filename base, e.g. "/dir/obj"
- RADOS (TODO): pool and object name base: "lsvd_pool/obj" 
- S3 (TODO): bucket name and object name base: "my_bucket/obj"

The `mkdisk.py` script creates a disk superblock, e.g.:
```
$ mkdir -p /tmp/dir/
$ python3 mkdisk.py /tmp/dir/obj
```

The `parse.py` script parses the superblock or other backend objects, for debugging purposes:
```
$ python3 parse.py /tmp/dir/obj
name:      /tmp/dir/obj
magic:     OK
UUID:      2a9e7b82-a4a6-11ec-95e3-2dc9b17b86ce
version:   1
type:      SUPER
seq:       0
n_hdr:     8
n_data:    0
vol_size:       20480
total_sectors:  0
live_sectors:   0
ckpts_offset:   0
ckpts_len:      0
clones:         [tbd]
snaps:          [tbd]
```

### SSD cache

The SSD cache is split into a read cache and a write cache, and is configured with `mkcache.py`. If you give it an existing file or partition it will calculate what I think are reasonable defaults.

**Cache sizing:** The write journal gets roughly 1/3 of the total cache size. We have two constraints on its size:
1. Since the translation layer buffers data in RAM, the write cache needs to hold the currently buffered object (8MB by default) plus any outstanding writes to NVMe, which we limit to half the cache size - i.e. a min write cache size of 16MB, or a total cache size of about 48MB.
1. Since outstanding writes are limited to 1/2 write cache size, the cache has to be double the largest write; QEMU has been observed to write 16MB in one operation, giving a min write cache size of 32MB, and a total size of about 100MB.

## Tests

- test1.py .. test5.py - really basic unit tests using ctypes
- test6.py - replays a trace from installing ubuntu on KVM/QEMU
- test7.cc - simple random writes, read back and verify CRC
- test8.cc - same, but with multiple threads
- test10.cc - crash test - forked writer calls exit, then parent verifies image

## To-do list

To do in the near term
- write coalescing
- LRU-ish read cache replacement
- `io_uring` for reading and writing cache
-  evict data from write cache to read cache, instead of discarding

Lower priority:
- random starting sequence in `mkcache.py`

Longer-term:
- fix the god-awful locking mess
- incremental map checkpoints [maybe]
- get the extent map working with unsigned ints, get rid of the 'A' field
- snapshots
- clones
- cache sharing across disk images
- see if there's a way to get data to the read cache without copying
