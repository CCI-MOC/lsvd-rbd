# Log Structured Virtual Disk

Described in the paper *Beating the I/O Bottleneck: A Case for Log-Structured Virtual Disks*, by Mohammad Hajkazemi, Vojtech Aschenbrenner, Mania Abdi, Emine Ugur Kaynar, Amin Mossayebzadeh, Orran Krieger, and Peter Desnoyers; EuroSys 2022

Uses two components: a local SSD partition (or file) and a remote backend store. (RADOS or S3)

The debug version uses files in a directory rather than an object store.

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

### Example

```
$ rm /mnt/nvme/lsvd/*
$ dd if=/dev/zero bs=1024k count=100 of=/mnt/nvme/lsvd/SSD status=none
$ python3 mkdisk.py --size 1g /mnt/nvme/lsvd/obj
$ python3 parse.py /mnt/nvme/lsvd/obj | grep uuid
$ python3 parse.py /mnt/nvme/lsvd/obj | grep UUID
UUID:      43f844ba-a4a5-11ec-95e3-2dc9b17b86ce
$ python3 mkcache.py --uuid 43f844ba-a4a5-11ec-95e3-2dc9b17b86ce /mnt/nvme/lsvd/SSD
$ make liblsvd.so
    ...
$ LD_PRELOAD=$PWD/liblsvd.so qemu-img dd if=rbd://mnt/nvme/lsvd/SSD,/mnt/nvme/lsvd/obj bs=64k count=10k of=file:///tmp/z1
$  ls -lh /tmp/z1
-rw-r--r-- 1 pjd pjd 640M Mar 15 21:19 /tmp/z1
```

## To-do list

To do in the near term
- RADOS and S3 backends, 
- corresponding RADOS and S3 mods to `mkdisk.py` (and optionally `parse.py`)
- basic greedy garbage collection
- write coalescing
- LRU-ish read cache replacement
- `io_uring` for reading and writing cache
-  evict data from write cache to read cache, instead of discarding

Lower priority:
- random starting sequence in `mkcache.py`
- check cache vs volume UUID 
- add local cache write sequence to backend object header, recover backend image from local cache on restart

Longer-term:
- fix the god-awful locking mess
- incremental map checkpoints
- get the extent map working with unsigned ints, get rid of the 'A' field
- snapshots
- clones
- cache sharing across disk images
- see if there's a way to get data to the read cache without copying
