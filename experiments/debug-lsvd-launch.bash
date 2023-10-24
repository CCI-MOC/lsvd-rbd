#!/usr/bin/env bash

set -euo pipefail
set -x

if [ "$EUID" -ne 0 ]
  then echo "Please run as root"
  exit
fi

if [ -z "${1:-}" ]
  then echo "Please provide a pool name"
  exit
fi

spdk_dir=${spdk_dir:-/home/isaackhor/code/spdk/}
lsvd_dir=${lsvd_dir:-/home/isaackhor/code/lsvd-rbd/}
experiment_dir=$lsvd_dir/experiments/
results_dir=$lsvd_dir/experiments/results/

blocksize=4096
imgsize=20g
imgname="lsvd-benchmark-$imgsize"

cd $lsvd_dir
make clean
make -j20 debug

cd $spdk_dir
scripts/rpc.py spdk_kill_instance SIGTERM > /dev/null || true
scripts/rpc.py spdk_kill_instance SIGKILL > /dev/null || true
pkill -f nvmf_tgt || true
pkill -f reactor_0 || true

mkdir -p /mnt/nvme/lsvd-rcache /mnt/nvme/lsvd-wcache
export LSVD_RCACHE_DIR=/mnt/nvme/lsvd-rcache
export LSVD_WCACHE_DIR=/mnt/nvme/lsvd-wcache
export LSVD_GC_THRESHOLD=40
###export fetch_window=0 # number of cache block fills -- 0 is bypass cache
gdb --args env LD_PRELOAD=/lib/x86_64-linux-gnu/libasan.so.8:$lsvd_dir/liblsvd.so ./build/bin/nvmf_tgt

exit

pool_name=triple-ssd
imgname=lsvd-debug
blocksize=4096
cd /home/isaackhor/code/spdk/
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10.1.[0-9.]+)\/24/')
scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
scripts/rpc.py bdev_rbd_create $pool_name $pool_name/$imgname $blocksize -c rbd_cluster
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Ceph0
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a $gw_ip -s 9922

