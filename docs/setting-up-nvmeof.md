# How to set up LSVD with Ceph NVMEoF

## High-level overview

Ceph NVMEoF https://github.com/ceph/ceph-nvmeof exports a RBD block image as
a NVME block device. We can fake the RBD block image with LSVD, intercepting
the RBD calls and forwarding them to LSVD via `LD_PRELOAD`.

## Steps

1. Build and install Ceph NVMEoF

```bash
# Clone the repo. Include submodules to get SPDK included in the repo
git clone --recurse-submodules https://github.com/ceph/ceph-nvmeof.git
cd ceph-nvmeof

# Set up grpc
make setup; make grpc

# Setup SPDK
cd spdk
sudo ./scripts/pkgdep.sh
sudo apt install -y librbd-dev
./configure --with-rbd
make -j10

# Set up huge pages for SPDK
sudo sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'
```

2. Configure ceph-nvmeof. Change the following values:

```
[gateway]
addr = <desired IP address of the NVMEoF target>
port = <port at which we reach the NVMEoF target>
enable_auth = False # True for secure ports

[ceph]
pool = <name of pool>

[spdk]
spdk_path = <full path to spdk directory, should be ./spdk>
tgt_path = build/bin/nvmf_tgt
```

3. Build LSVD

```bash
git clone https://github.com/CCI-MOC/lsvd-rbd
cd lsvd-rbd
make -j10 release # debug for debug build. debug build is not optimised and has sanitisers enabled
```

4. Create LSVD image

```bash
# syntax: ./imgtool --create --rados --size=10g <pool name>/<prefix>
./imgtool --create --rados --size=10g rbd/fio-target
```

5.  Start up the server

```bash
# In the ceph-nvmeof directory. replace lsvd.conf with path to nvmeof conf file
# This starts up the control server
# LD_PRELOAD to hijack rbd with LSVD
sudo LD_PRELOAD=<path/to/liblsvd.so> PYTHONPATH=./spdk/python/\ 
        LSVD_RCACHE_DIR=<read/cache/dir> LSVD_WCACHE_DIR=<write/cache/dir>\
        python3 -m control -c ./lsvd.conf

# use -i <name used in imgtool earlier>
python3 -m control.cli create_bdev -i rbd/fio-target -p rbd -b Ceph0
python3 -m control.cli create_subsystem -n nqn.2016-06.io.spdk:cnode1 -s SPDK00000000000001
python3 -m control.cli add_namespace -n nqn.2016-06.io.spdk:cnode1 -b Ceph0
python3 -m control.cli add_host -n nqn.2016-06.io.spdk:cnode1 -t '*'
python3 -m control.cli create_listener -n nqn.2016-06.io.spdk:cnode1 -s 5001 # replace with your favourite port
```

6. Connect to the nvmeof target

```bash
# on client machine
sudo apt -y install nvme-cli
sudo modprobe nvme-fabrics

# look for the device
sudo nvme discover -t tcp -a 10.1.0.5 -s 5001 # replace with your server IP and port

# connect
sudo nvme connect -t tcp --traddr 10.1.0.5 -s 5001 -n nqn.2016-06.io.spdk:cnode1
```

7. The nvmeof device should now be available; view it with `nvme list`
