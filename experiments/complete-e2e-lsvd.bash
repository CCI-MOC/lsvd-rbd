#!/usr/bin/env bash

set -euo pipefail

# pool must already exist
if [ -z "$1" ]
  then echo "Please provide a pool name"
  exit
fi
pool_name=$1

cur_time=$(date +"%FT%T")
spdk_dir=/home/pjd/spdk/
lsvd_dir=/home/pjd/new-stuff/
#experiment_dir=$(pwd)
experiment_dir=/home/pjd/new-stuff/experiments/
results_dir=$experiment_dir/results
outfile=$experiment_dir/results/$cur_time.txt

gateway_host=dl380p-5
client_host=dl380p-6


# Build LSVD
cd $lsvd_dir
# make clean
make -j10 release
# make -j10 debug
make imgtool
make thick-image

# setup spdk
cd $spdk_dir
# sudo ./scripts/pkgdep.sh
# ./configure --with-rbd
# make -j10
sudo sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'

# start server
mkdir -p /mnt/nvme/lsvd_rcache /mnt/nvme/lsvd_wcache

echo killing old SPDK...
sudo scripts/rpc.py spdk_kill_instance SIGTERM || true
sudo pkill nvmf_tgt || true
sudo pkill reactor_0 || true
sleep 5
ps aux |grep reactor_0

echo starting SPDK...
sudo LD_PRELOAD=$lsvd_dir/liblsvd.so \
	LSVD_RCACHE_DIR=/mnt/nvme/lsvd_rcache/ \
	LSVD_WCACHE_DIR=/mnt/nvme/lsvd_wcache \
	LSVD_GC_THRESHOLD=40 \
	./build/bin/nvmf_tgt &

# == setup spdk target ===

# Create the image
IMG=$cur_time
echo Making image...
$lsvd_dir/thick-image --size=10g $pool_name/$IMG

printf "\n\n\n===Starting automated test on pool: $pool_name===\n\n" >> $outfile

# setup the bdevs. the image must already exist
sudo scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
# syntax: bdev_rbd_create <name> <pool> <image-name> <block_size> [-c <cluster_name>]
# image must already exist
sudo scripts/rpc.py bdev_rbd_create $pool_name $pool_name/$IMG 4096 -c rbd_cluster

# create subsystem
sudo scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
sudo scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Ceph0

# setup listener
sudo scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 10.1.0.5 -s 9922

# == run benchmark and collect results ===
cd $experiment_dir
ssh $client_host "sudo bash -s iodepth=128 blksize=4k" < client-bench.bash 2>&1 | tee -a $outfile
ssh $client_host "sudo bash -s iodepth=256 blksize=4k" < client-bench.bash 2>&1 | tee -a $outfile
ssh $client_host "sudo bash -s iodepth=128 blksize=16k" < client-bench.bash 2>&1 | tee -a $outfile

# cleanup
sudo ./imgtool --delete --rados $pool_name/$IMG
