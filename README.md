# Log Structured Virtual Disk

Described in the paper *Beating the I/O Bottleneck: A Case for Log-Structured Virtual Disks*, by Mohammad Hajkazemi, Vojtech Aschenbrenner, Mania Abdi, Emine Ugur Kaynar, Amin Mossayebzadeh, Orran Krieger, and Peter Desnoyers; EuroSys 2022

Uses two components: a local SSD partition (or file) and a remote backend store. (RADOS or S3)

The debug version uses files in a directory rather than an object store.

## status
As of 11/28, there seem to be two remaining bugs:
- occasional read hang, observed 2x on test10 and 1x on QEMU
- something to do with the read cache - occasionally returns data from the wrong object at the same offset (mod 128 sector block size) as the target sector. Reliably reproducible with test10
- [FIXED] QEMU regression - abort in malloc/free in batch::~batch
- QEMU regression - seems related to the hanging reads

## usage - QEMU

Pick a RADOS pool, an image name, and a directory on an SSD-based file system (`rbd`, `img1`, and `/mnt/nvme/lsvd` in the example below).

1. create a disk. This creates a "superblock" - a 4KB RADOS object named `img1`, in this case, holding volume metadata:

    `sudo python3 mkdisk.py --rados --size 10g rbd/img1`

2. Set your configuration. The config variables are (sort of) described in `config.h`; you can either (a) set them in `lsvd.conf` or (b) pass them as environment variables. 

3. start KVM using the virtual disk. Here we're starting a 2 VCP instance with 1G of RAM, a CD drive running off an ISO image, SSH on localhost:5555, and our virtual drive:
```
sudo env LSVD_CACHE_DIR=/mnt/nvme/lsvd LSVD_CACHE_SIZE=1000m \
	LD_PRELOAD=$PWD/liblsvd.so XAUTHORITY=$HOME/.Xauthority\
    qemu-system-x86_64 -m 1024 -cdrom whatever.iso \
    -blockdev '{"driver":"rbd","pool":"rbd","image":"rbd/img1","server":[{"host":"10.1.0.4","port":"6789"}],"node-name":"libvirt-2-storage","auto-read-only":true,"discard":"unmap"}' -device virtio-blk,drive=libvirt-2-storage \
    -device e1000,netdev=net0 -netdev user,id=net0,hostfwd=tcp::5555-:22 \
    -k en-us -machine accel=kvm -smp 2 -m 1024
```

Unit tests included here:
- test1.py through test5.py - really simple functional tests
- test6.py - replays a trace from the Ubuntu 14 install
- test7.cc - random writes and reads followed by readback, with CRC verification
- test8.cc - random writes from multiple threads, CRC verification
- test10.cc - similar, but crash and restart

If you want to compare it with a VM running off an image file on your local SSD:
```
qemu-img create -f qcow2 /mnt/nvme/qcow/img1.img 20G
sudo env XAUTHORITY=$HOME/.Xauthority \
    qemu-system-x86_64 -m 1024 -cdrom whatever.iso \
    -drive file=/mnt/nvme/qcow/img1.img,media=disk,if=virtio \
	-device e1000,netdev=net0 -netdev user,id=net0,hostfwd=tcp::5555-:22 \
	-k en-us -machine accel=kvm -smp 2 -m 1024
```

### Notes on erasure-coded pools

Because of its read cache, LSVD makes lots of 64KB random reads to the backend. I believe (but haven't fully verified) that it's more efficient if this matches the stripe size of your erasure code - e.g.
```
sudo ceph osd erasure-code-profile set ec83b k=8 m=3 plugin=isa \
    technique=cauchy crush-failure-domain=osd crush-root=default~ssd \
    stripe-unit=64k
sudo ceph osd pool create ec83b_pool erasure ec83b
```

(yeah, crush-failure-domain=osd is sketchy, but I only have 4 OSD servers in my test cluster...)

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
