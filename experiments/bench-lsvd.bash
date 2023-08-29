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
spdk_dir=/home/pjd/spdk/
lsvd_dir=/home/pjd/new-stuff/
experiment_dir=/home/pjd/new-stuff/experiments/
results_dir=$experiment_dir/results/
outfile=$experiment_dir/results/$cur_time.$pool_name.lsvd.txt

gateway_host=dl380p-5
client_host=dl380p-6

# Build LSVD
cd $lsvd_dir
# make clean
make -j20 release
# make -j10 debug
make imgtool
make thick-image

IMG=$cur_time
# Create the image
#./imgtool --create --rados --size=10g $pool_name/$cur_time
./thick-image --size=10g $pool_name/$IMG
#./imgtool --create --rados --size=10g rbd/fio-target

# setup spdk
cd $spdk_dir
# ./scripts/pkgdep.sh
# ./configure --with-rbd
# make -j10
sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'

# start server
mkdir -p /mnt/nvme/lsvd-rcache /mnt/nvme/lsvd-wcache
export LD_PRELOAD=$lsvd_dir/liblsvd.so
export LSVD_RCACHE_DIR=/mnt/nvme/lsvd-rcache
export LSVD_WCACHE_DIR=/mnt/nvme/lsvd-wcache
export LSVD_GC_THRESHOLD=40
scripts/rpc.py spdk_kill_instance SIGTERM > /dev/null || true
pkill nvmf_tgt || true
pkill reactor_0 || true
sleep 5
./build/bin/nvmf_tgt &

# == setup spdk target ===
printf "\n\n\n===Starting automated test on pool: $pool_name===\n\n" >> $outfile

# setup the bdevs. the image must already exist
scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
# syntax: bdev_rbd_create <name> <pool> <image-name> <block_size> [-c <cluster_name>]
# image must already exist
scripts/rpc.py bdev_rbd_create $pool_name $pool_name/$IMG 4096 -c rbd_cluster

# create subsystem
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Ceph0

# setup listener
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 10.1.0.5 -s 9922

# == run benchmark and collect results ===
#cd $experiment_dir
#scp ./filebench-*.txt root@$client_host:/tmp/
#ssh $client_host "bash -s" < client-bench.bash 2>&1 | tee -a $outfile

echo Hit ENTER to terminate...
read a

# cleanup
cd $spdk_dir
scripts/rpc.py bdev_rbd_delete Ceph0
scripts/rpc.py bdev_rbd_unregister_cluster rbd_cluster
scripts/rpc.py spdk_kill_instance SIGTERM
sleep 5
