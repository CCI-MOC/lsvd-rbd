# LSVD - Log-Structured Virtual Disk

Original paper [here](https://dl.acm.org/doi/10.1145/3492321.3524271)
ATC2024 submission is in the atc2024 submodule.

It's not quite as fast as the the version in the paper, but on my old machines
(IvyBridge) and Ceph 17.2.0 I'm getting about 100K write IOPS against an
erasure-coded pool on SATA SSDs with moderate CPU load on the backend (~30% CPU
* 32 OSDs) vs maybe 5x that for half as many IOPS with straight RBD and a
replicated pool.

Note that although individual disk performance is important, the main goal is to
be able to support higher aggregate client IOPS against a given backend OSD
pool.

## Stability

This is NOT production-ready code; it still occasionally crashes, and some
configurations cause runaway memory leaks.

It is able to install and boot Ubuntu 22.04 (see `qemu/`) and is stable under
most of our tests, but there are likely regressions around crash recovery and
other less well-trodden paths.

## How to run

Note that the examples here use the fish shell, that the local nvme cache is
`/dev/nvme0n1`, and that the ceph config files are available in `/etc/ceph`.

```
echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo docker run --net host -v /dev/hugepages:/dev/hugepages -v /etc/ceph:/etc/ceph -v /var/tmp:/var/tmp -v /dev/shm:/dev/shm -v /mnt/nvme0:/lsvd -i -t --privileged --entrypoint /usr/bin/fish ghcr.io/cci-moc/lsvd-rbd:main
```

If you run into an error, you might need to rebuild the image:

```
git clone https://github.com/cci-moc/lsvd-rbd.git
cd lsvd-rbd
docker build -t lsvd-rbd .
sudo docker run --net host -v /dev/hugepages:/dev/hugepages -v /etc/ceph:/etc/ceph -v /var/tmp:/var/tmp -v /dev/shm:/dev/shm -v /mnt/nvme0:/lsvd -i -t --privileged --entrypoint /usr/bin/fish lsvd-rbd
```

To start the gateway:

```
./build-rel/lsvd_tgt
```

The target will start listening to rpc commands on `/var/tmp/spdk.sock`.

To create an lsvd image on the backend:

```
#./imgtool create <pool> <imgname> --size 100g
./imgtool create lsvd-ssd benchtest1 --size 100g
```

To configure nvmf:

```
cd subprojects/spdk/scripts
./rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
./rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
./rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 0.0.0.0 -s 9922
```

To mount images on the gateway:

```
export PYTHONPATH=/app/src/
./rpc.py --plugin rpc_plugin bdev_lsvd_create lsvd-ssd benchtest1 -c '{"rcache_dir":"/lsvd","wlog_dir":"/lsvd"}'
./rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 benchtest1
```

To gracefully shutdown gateway:

```
./rpc.py --plugin rpc_plugin bdev_lsvd_delete benchtest1
./rpc.py spdk_kill_instance SIGTERM
docker kill <container id>
```

## Mount a client

Fill in the appropriate IP address:

```
modprobe nvme-fabrics
nvme disconnect -n nqn.2016-06.io.spdk:cnode1
export gw_ip=${gw_ip:-192.168.52.109}
nvme connect -t tcp  --traddr $gw_ip -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 2
nvme list
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $dev_name\n"
```

## Build and run without Docker

Start the gateway:

```
make debug
cd build-dbg
./lsvd
```

Configure RPC:

```
cd ../subprojects/spdk/scripts
export PYTHONPATH=../../../tools/
export gateway_ip=127.0.0.1
export img=lsvddev
./rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
./rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
./rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a $gateway_ip -s 9922
./rpc.py --plugin rpc_plugin bdev_lsvd_create pone $img -c 'config_here'
./rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $img
```

Connect to the target:

```
nvme connect -t tcp  --traddr localhost -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
```

Disconnect: 

```
nvme disconnect -n nqn.2016-06.io.spdk:cnode1
```


## Build

This project uses `meson` to manage the build system. Run `make setup` to
generate the build files, then run `meson compile` in either `build-rel` or
`build-dbg` to build the release or debug versions of the code.

A makefile is also offered for convenience; `make` builds the debug version
by default.

## Configuration

LSVD is configured using a JSON file. When creating an image, we will
try to read the following paths and parse them for configuration options:

- Default built-in configuration
- `/usr/local/etc/lsvd.json`
- `./lsvd.json`
- user supplied path

The file read last has highest priority.

We will also first try to parse the user-supplied path as a JSON object, and if
that fails try treat it as a path and read it from a file.

An example configuration file is provided in `docs/example_config.json`.

## Image and object names

Currently the name of an image is *pool/name*.

Given an image name of `mypool/disk_x`, LSVD stores a *superblock* in the object
`disk_x`, and a series of *data objects* and *checkpoint objects* in
`disk_x.00000001`, etc. using hexadecimal 32-bit sequence numbers. 

- superblock - holds volume size, clone linkage, [TBD snapshot info], and clean shutdown info / pointer to roll-forward point.
- data objects - data blocks and metadata (i.e. list of LBAs) for log recovery
- checkpoints - full map, persisted at shutdown and periodically to limit the amount of log recovery needed after a crash

[TODO - use a larger sequence number - at 100K IOPS, 2^32 gives us a bit less than 3 years]

[TODO - checkpoints can be large for large volumes, although we don't have good data points for realistic workloads. (The GC does defragmentation, and the map uses length encoded extents, so unless you run pure random writes it's hard to calculate the map size) 
We probably need to split the checkpoint into multiple objects and write it incrementally to handle multi-TB volumes]

## How it works

There's a more detailed (although possibly out-of-date) description of the internals in [review-notes.md](review-notes.md)

Writes: Incoming writes are optionally batched, if there's a queue for the NVMe device, and written in journal records to NVMe from within the calling thread.
A libaio completion thread passes the data to the translation layer, where it gets copied into a batch buffer, and then notifies callers.
Full batches are tossed on a queue for another thread, as lock delays in the translation layer tend to be a lot longer than in the write cache. 
They get written out and the new LBA to object/offset mapping is recorded in the object map.

Reads: this is more complicated, and described in some detail in [review-notes.md](review-notes.md).
We iterate over the requested address range, finding extents which are resident in the write cache, passing any other mapped extents to the read cache, and zeroing any unmapped extents, aggregating all the individual requests we've broken it up into.
Then we launch all the requests and wait for them to complete.

Completions come from the write cache libaio callback thread, the read cache callback thread, or RADOS aio notification.

(note that heavily fragmented reads are atypical, but of course have to be handled properly for correctness)

## Garbage collection

LSVD currently uses a fairly straightforward greedy garbage collector, selecting objects with utilization (i.e. fraction of live sectors) below a certain threshold and cleaning them.
This threshold can be set (as a percent, out of 100) with the `gc_threshold` parameter.
Higher values will reduce wasted space, while lower values will decrease 
As a rough guide, overall volume utilization will be halfway between this threshold and 100% - e.g. a value of 60 should give an overall utilization of about 80%, or a space expansion of 25% (i.e. 100/80). 

Raising this value above about 70 is not recommended, and we would suggest instead using lower-rate erasure codes - e.g. an 8,2 code plus 25% overhead (i.e. gc=60) gives an overall space expansion of 1.56x, vs 3x for triple replication.

The GC algorithm runs in fairly large batches, and stores data in a temporary file in the cache directory rather than buffering it in memory.
At the moment we don't have any controls on the size of this file or mechanism for limiting it, although in practice it's been reasonably small compared to the cache itself. [TODO]

## Cache consistency

The write cache can't be decoupled from the virtual disk, but there are a number of mechanisms to ensure consistency and some parameters to tweak them.

1. Write ordering - write order is maintained through to the backend, which is important for some of the recovery mechanisms below
1. Post-crash cache recovery - the write cache is a journal, and after recovery we replay any writes which didn't make it to the backend.
1. Cache loss - *prefix consistency* is maintained - if any write is visible in the image, all writes preceding that are visible. This ensures file system consistency, but may lose data.
1. Write atomicity - writes up to 2MB are guaranteed to be atomic. Larger ones are split into 2MB chunks, and it's possible that the tail of such a sequence could be lost while the first chunks are recorded. 

The `hard_flush` configuration option will cause flush operations to push all data to the backend, at a substantial loss in performance for sync-heavy workloads. Alternately the `flush_msec` option (default 2s) can be used to bound the duration of possible data loss.

Note that `lsvd_crash_test` simulates crashes by killing a subprocess while it's writing, and optionally deletes the cache before recovery.
To verify consistency writes are stamped with sequence numbers, and the test finds the last sequence number present in the image and verifies that all preceding writes are fully present as well.

## Erasure code notes

The write logic creates large objects (default 8MB), so it works pretty well on pretty much any pool.
The read cache typically fetches 64K blocks, so there may be a bit of extra load on the backend if you use the default 4KB stripe size instead of an  erasure code with a stripe size of 64K; however I haven't really tested how much of a difference this makes.
Most of the testing to date has been with an 8,3 code with 64K stripe size.

## Tools

```
build$ ./imgtool --help
❯ ./imgtool --help
Allowed options:
  --help                produce help message
  --cmd arg             subcommand: create, clone, delete, info
  --img arg             name of the iname
  --pool arg            pool where the image resides
  --size arg (=1G)      size in bytes (M=2^20,G=2^30)
  --dest arg            destination (for clone)
```

Other tools live in the `tools` subdirectory - see the README there for more details.
