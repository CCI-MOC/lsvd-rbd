# LSVD - Log-Structured Virtual Disk

Original paper [here](https://dl.acm.org/doi/10.1145/3492321.3524271).

## How to run

Note that the examples here use the fish shell, that the local nvme cache is
`/dev/nvme0n1`, and that the ceph config files are available in `/etc/ceph`.

First, create an lsvd image on the backend:

```
#./imgtool create <pool> <imgname> --size 100g
./imgtool create lsvd-ssd benchtest1 --size 100g
```

```
echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo docker run --net host \
    -v /dev/hugepages:/dev/hugepages \
    -v /etc/ceph:/etc/ceph \
    -v /var/tmp:/var/tmp \
    -v /dev/shm:/dev/shm \
    -v /mnt/nvme0:/mnt/lsvd \
    -i -t --privileged \
    --entrypoint /app/build-rel/lsvd \
    ghcr.io/cci-moc/lsvd-rbd:main
```

If you run into an error, you might need to rebuild the image on your machine:

```
git clone https://github.com/cci-moc/lsvd-rbd.git
cd lsvd-rbd
docker build -t lsvd-rbd .
sudo docker run --net host \
    -v /dev/hugepages:/dev/hugepages \
    -v /etc/ceph:/etc/ceph \
    -v /var/tmp:/var/tmp \
    -v /dev/shm:/dev/shm \
    -v /mnt/nvme0:/mnt/lsvd \
    -i -t --privileged \
    --entrypoint /app/build-rel/lsvd \
    lsvd-rbd
```

To start the gateway:

```
./build-rel/lsvd
```

The target will start listening to rpc commands on `/var/tmp/spdk.sock`.
The gateway accepts three notable flags:

```
lsvd: Usage: lsvd_tgt [none|mount|new] [args]

  Flags from ../src/main.cc:
    -lsvd_cache_nvm (NVM cache size in GiB) type: uint64 default: 100
    -lsvd_cache_path (Path to lsvd read cache) type: string
      default: "/mnt/lsvd/lsvd.rcache"
    -lsvd_cache_ram (RAM cache size in GiB) type: uint64 default: 10
```

The gateway also has some helpful command line arguments:

- `./lsvd new <pool> <num>` will create `<num>` images in `<pool>` and with
  names `auto_lsvddev_<num>` and mount them (deleting them if one already
  exists and creating a now one in its place).
- `./lsvd mount <pool> <name>` will mount the image `<name>` in `<pool>`.

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
./rpc.py --plugin rpc_plugin bdev_lsvd_create lsvd-ssd benchtest1 -c 'TODO'
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

## Tools

The `imgtool` tool is a simple command line utility to create, delete,
and clone images:

```
❯ ./imgtool --help
Allowed options:
  -h [ --help ]           produce help message
  -c [ --cmd ] arg        subcommand: create, clone, delete, info, objinfo
  -i [ --img ] arg        name of the iname
  -p [ --pool ] arg       pool where the image resides
  -s [ --size ] arg (=1G) size in bytes (M=2^20,G=2^30)
  -q [ --seq ] arg        seqnum for objinfo
  -x [ --hex ] arg (=0)   hexdump for objinfo
  --thick arg (=0)        thick provision when creating an image (not currently
                          supported)
  --dest arg              destination (for clone)
```

Example:

```
./imgtool create pool1 img1 -s 1g
./imgtool delete -p pool2 -i img1 -s 1g
./imgtool info pool2 img2
./imgtool objinfo pool2 img2 -q 333
```

In cases of severe image corruption, it may be neccessary to completely wipe
an image with an external tool instead. The `remove_objs.py` script will scan
through the entire pool for objects with a given prefix and remove them.

```
./remove_objs.py pool1 prefix
```

To help with debugging, the `bench_automount.bash` script will automatically
mount the lsvd nvme controller and run a fio workload against all the namespaces
on the controller. It is intended to be used together with the `new` mode
of the gateway (which automatically creates and mounts images).

```
./lsvd new pool1 3 # creates 3 images in pool1

# in another terminal
./tools/bench_automount.bash tools/write_only.fio
```
