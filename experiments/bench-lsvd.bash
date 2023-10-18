#!/usr/bin/env bash

set -euo pipefail

if [ "$EUID" -ne 0 ]
  then echo "Please run as root"
  exit
fi

if [ -z "${1:-}" ]
  then echo "Please provide a pool name"
  exit
fi

# pool must already exist
pool_name=$1
cur_time=$(date +"%FT%T")

spdk_dir=${spdk_dir:-/home/isaackhor/code/spdk/}
lsvd_dir=${lsvd_dir:-/home/isaackhor/code/lsvd-rbd/}
experiment_dir=$lsvd_dir/experiments/
results_dir=$lsvd_dir/experiments/results/
outfile=$results_dir/$cur_time.$pool_name.lsvd.txt

gw_ip=$(ip addr | grep inet | fgrep inet\ 10.1 | \
	   sed -e 's/.*inet //' -e 's,/.*,,')
echo GATEWAY IP=$gw_ip
client_ip=${client_ip:-10.1.0.6}

# imgname=$cur_time
# imgname=prealloc-80g # pre-allocated thick 80g image on pool 'triple-ssd'
imgname=lsvd-benchmark-thick-80g
blocksize=4096

# Build LSVD
echo '===Building LSVD...'
cd $lsvd_dir
make clean
make -j20 release
# make -j10 debug
make -j20 imgtool
make -j20 thick-image

# Create the image
# only re-provision when we start a new run to preserve previous image for debugging
./imgtool --delete --rados $pool_name/$imgname || true
./thick-image --size=80g $pool_name/$imgname
#./imgtool --create --rados --size=10g $pool_name/$cur_time

# setup spdk
cd $spdk_dir
# ./scripts/pkgdep.sh
# ./configure --with-rbd
# make -j10
sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'

# kill existing spdk instances
echo '===Killing existing spdk instances...'
scripts/rpc.py spdk_kill_instance SIGTERM > /dev/null || true
scripts/rpc.py spdk_kill_instance SIGKILL > /dev/null || true
pkill -f nvmf_tgt || true
pkill -f reactor_0 || true
sleep 5

# start server
echo '===Starting spdk...'
mkdir -p /mnt/nvme/lsvd-rcache /mnt/nvme/lsvd-wcache
export LD_PRELOAD=$lsvd_dir/liblsvd.so
export LSVD_RCACHE_DIR=/mnt/nvme/lsvd-rcache
export LSVD_WCACHE_DIR=/mnt/nvme/lsvd-wcache
export LSVD_GC_THRESHOLD=40
###export fetch_window=0 # number of cache block fills -- 0 is bypass cache
./build/bin/nvmf_tgt &

# == setup spdk target ===
printf "\n\n\n===Starting automated test on pool: $pool_name===\n\n" >> $outfile

scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
# syntax: bdev_rbd_create <name> <pool> <image-name> <block_size> [-c <cluster_name>]
# image must already exist
scripts/rpc.py bdev_rbd_create $pool_name $pool_name/$imgname $blocksize -c rbd_cluster
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Ceph0
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a $gw_ip -s 9922

# == run benchmark and collect results ===
cd $experiment_dir
ssh $client_ip 'mkdir -p /tmp/filebench'
scp ./filebench-workloads/*.f root@$client_ip:/tmp/filebench/
ssh $client_ip "bash -s gw_ip=$gw_ip iodepth=256" < client-bench.bash 2>&1 | tee -a $outfile

perl -lane 'print if s/RESULT: //' $outfile | tee -a $outfile

# cleanup
cd $spdk_dir
scripts/rpc.py bdev_rbd_delete Ceph0
scripts/rpc.py bdev_rbd_unregister_cluster rbd_cluster
scripts/rpc.py spdk_kill_instance SIGTERM
sleep 5

cd $lsvd_dir
# ./imgtool --delete --rados $pool_name/$imgname
