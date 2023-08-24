#!/usr/bin/env bash

set -euo pipefail

# pool must already exist
pool_name=rbd

spdk_dir=/home/isaackhor/code/spdk/
lsvd_dir=/home/isaackhor/code/lsvd-rbd/
experiment_dir=$(pwd)
results_dir=$experiment_dir/results

gateway_host=dl380p-5
client_host=dl380p-6

cur_time=$(date --iso8601=seconds)

# Build LSVD
cd $lsvd_dir
# make clean
make -j10 release
# make -j10 debug
make imgtool

# Create the image
./imgtool --create --rados --size=10g $pool_name/$cur_time
#./imgtool --create --rados --size=10g rbd/fio-target

# setup spdk
cd $spdk_dir
# sudo ./scripts/pkgdep.sh
# ./configure --with-rbd
# make -j10
sudo sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'

# start server
mkdir -p /tmp/lsvd_rcache /tmp/lsvd_wcache
sudo LD_PRELOAD=$lsvd_dir/liblsvd.so \
	LSVD_RCACHE_DIR=/tmp/lsvd_rcache/ \
	LSVD_WCACHE_DIR=/tmp/lsvd_wcache \
	./build/bin/nvmf_tgt &

# == setup spdk target ===

# setup the bdevs. the image must already exist
scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
# syntax: bdev_rbd_create <name> <pool> <image-name> <block_size> [-c <cluster_name>]
# image must already exist
scripts/rpc.py bdev_rbd_create $pool_name $pool_name/$cur_time 4096 -c rbd_cluster

# create subsystem
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Ceph0

# setup listener
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 10.1.0.5 -s 9922

# == run benchmark and collect results ===
cd $experiment_dir
ssh $client_host -i ~/.ssh/id_northeastern "bash -s" < client-bench.bash > results/$cur_time.txt 2>&1
