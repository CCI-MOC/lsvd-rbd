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

## what's here

This builds `liblsvd.so`, which provides most of the basic RBD API; you can use
`LD_PRELOAD` to use this in place of RBD with `fio`, KVM/QEMU, and a few other
tools. It also includes some tests and tools described below.

The repository also includes scripts to setup a SPDK NVMeoF target.

## Stability

This is NOT production-ready code; it still occasionally crashes, and some
configurations cause runaway memory leaks.

It is able to install and boot Ubuntu 22.04 (see `qemu/`) and is stable under
most of our tests, but there are likely regressions around crash recovery and
other less well-trodden paths.

## How to run

```
echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
docker run --net host -v /dev/hugepages:/dev/hugepages -v /etc/ceph:/etc/ceph -v /var/tmp:/var/tmp -v /dev/shm:/dev/shm -i -t --privileged --entrypoint /bin/bash ghcr.io/cci-moc/lsvd-rbd:main
```

If the cpu is too old, you might have to rebuild the image:

```
git clone https://github.com/cci-moc/lsvd-rbd.git
cd lsvd-rbd
docker build -t lsvd-rbd .
docker run --net host -v /dev/hugepages:/dev/hugepages -v /etc/ceph:/etc/ceph -v /var/tmp:/var/tmp -v /dev/shm:/dev/shm -i -t --privileged --entrypoint /bin/bash lsvd-rbd
```

To setup lsvd images:

```
#./imgtool create <pool> <imgname> --size 100g
./imgtool create lsvd-ssd benchtest1 --size 100g
```

To configure nvmf:

```
export gateway_ip=0.0.0.0
./rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
./rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
./rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a $gateway_ip -s 9922
```

To mount images on the gateway:

```
export PYTHONPATH=/app/src/
./rpc.py --plugin rpc_plugin bdev_lsvd_create lsvd-ssd benchtest1 -c '{"rcache_dir":"/var/tmp/lsvd","wlog_dir":"/var/tmp/lsvd"}'
./rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 benchtest1
```

To kill gracefully shutdown gateway:

```
./rpc.py --plugin rpc_plugin bdev_lsvd_delete benchtest1
./rpc.py spdk_kill_instance SIGTERM
docker kill <container id>
```

## Mount a client

```
modprobe nvme-fabrics
nvme disconnect -n nqn.2016-06.io.spdk:cnode1
gw_ip=${gw_ip:-10.1.0.5}
nvme connect -t tcp  --traddr $gw_ip -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 2
nvme list
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $dev_name\n"
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

## Usage

### Running SPDK target

You might need to enable hugepages:
```
sudo sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'
```

Now we start the target, with or without `LD_PRELOAD`, potentially under the debugger. Run `spdk_tgt --help` for more options - in particular, the RPC socket defaults to `/var/tmp/spdk.sock` but a different one can be specified, which might allow running multiple instances of SPDK. Also the roc command has a `—help` option, which is about 500 lines long.

```
SPDK=/mnt/nvme/ceph-nvmeof/spdk
sudo LD_PRELOAD=$PWD/liblsvd.so $SPDK/build/bin/spdk_tgt
```

Here's a simple setup - the first two steps are handled in the ceph-nvmeof python code, and it may be worth looking through the code more to see what options they use.

```
sudo $SPDK/scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
sudo $SPDK/scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
sudo $SPDK/scripts/rpc.py bdev_rbd_create rbd rbd/fio-target 4096 -c rbd_cluster
sudo $SPDK/scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
sudo $SPDK/scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Ceph0
sudo $SPDK/scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 10.1.0.8 -s 5001
```

Note also that you can create a ramdisk test, by (1) creating a ramdisk with brd, and (2) creating another bdev / namespace with `bdev_aio_create`. With the version of SPDK I have, it does 4KB random read/write at about 100K IOPS, or at least it did, a month or two ago, on the HP machines.

Finally, I’m not totally convinced that the options I used are the best ones - the -u/-m/-c options for `create_transport` were blindly copied from a doc page. I’m a little more convinced that specifying a 4KB block size in `dev_rbd_create` is a good idea.

## Tests

There are two tests included: `lsvd_rnd_test` and `lsvd_crash_test`. 
They do random writes of various sizes, with random data, and each 512-byte sector is "stamped" with its LBA and a sequence number for the write.
CRCs are saved for each sector, and after a bunch of writes we read everything back and verify that the CRCs match.

### `lsvd_rnd_test`

```
build$ bin/lsvd_rnd_test --help
Usage: lsvd_rnd_test [OPTION...] RUNS

  -c, --close                close and re-open
  -d, --cache-dir=DIR        cache directory
  -D, --delay                add random backend delays
  -k, --keep                 keep data between tests
  -l, --len=N                run length
  -O, --rados                use RADOS
  -p, --prefix=PREFIX        object prefix
  -r, --reads=FRAC           fraction reads (0.0-1.0)
  -R, --reverse              reverse NVMe completion order
  -s, --seed=S               use this seed (one run)
  -v, --verbose              print LBAs and CRCs
  -w, --window=W             write window
  -x, --existing             don't delete existing cache
  -z, --size=S               volume size (e.g. 1G, 100M)
  -Z, --cache-size=N         cache size (K/M/G)
  -?, --help                 Give this help list
      --usage                Give a short usage message
```

Unlike the normal library, it defaults to storing objects on the filesystem; the image name is just the path to the superblock object (the --prefix argument), and other objects live in the same directory.
If you use this, you probably want to use the `--delay` flag, to have object read/write requests subject to random delays.
It creates a volume of --size bytes, does --len random writes of random lengths, and then reads it all back and checks CRCs. 
It can do multiple runs; if you don't specify --keep it will delete and recreate the volume between runs. 
The --close flag causes it to close and re-open the image between runs; otherwise it stays open.

### `lsvd_rnd_test`

This is pretty similar, except that does the writes in a subprocess which kills itself with `_exit` rather than finishing gracefully, and it has an option to delete the cache before restarting.

This one needs to be run with the file backend, because some of the test options crash the writer, recover the image to read and verify it, then restore it back to its crashed state before starting the writer up again.

It uses the write sequence numbers to figure out which writes made it to disk before the crash, scanning all the sectors to find the highest sequence number stamp, then it veries that the image matches what you would get if you apply all writes up to and including that sequence number.

```
build$ bin/lsvd_crash_test --help
Usage: lsvd_crash_test [OPTION...] RUNS

  -2, --seed2                seed-generating seed
  -d, --cache-dir=DIR        cache directory
  -D, --delay                add random backend delays
  -k, --keep                 keep data between tests
  -l, --len=N                run length
  -L, --lose-writes=N        delete some of last N cache writes
  -n, --no-wipe              don't clear image between runs
  -o, --lose-objs=N          delete some of last N objects
  -p, --prefix=PREFIX        object prefix
  -r, --reads=FRAC           fraction reads (0.0-1.0)
  -R, --reverse              reverse NVMe completion order
  -s, --seed=S               use this seed (one run)
  -S, --sleep                child sleeps for debug attach
  -v, --verbose              print LBAs and CRCs
  -w, --window=W             write window
  -W, --wipe-cache           delete cache on restart
  -x, --existing             don't delete existing cache
  -z, --size=S               volume size (e.g. 1G, 100M)
  -Z, --cache-size=N         cache size (K/M/G)
  -?, --help                 Give this help list
      --usage                Give a short usage message
```
