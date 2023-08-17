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