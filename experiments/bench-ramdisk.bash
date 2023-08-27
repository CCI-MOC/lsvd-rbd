#!/usr/bin/env bash

set -euo pipefail

if [ "$EUID" -ne 0 ]
  then echo "Please run as root"
  exit
fi

# kill children when we die, otherwise nvmf_tgt will keep running
# trap "trap - SIGTERM && kill -- $$" SIGINT SIGTERM EXIT

cur_time=$(date --iso-8601=seconds)
spdk_dir=/home/isaackhor/code/spdk/
lsvd_dir=/home/isaackhor/code/lsvd-rbd/
experiment_dir=/home/isaackhor/code/lsvd-rbd/experiments/
results_dir=$experiment_dir/results/
outfile=$experiment_dir/results/$cur_time.txt

gateway_host=dl380p-5
client_host=dl380p-6

printf "\n\n\n===Starting automated test on ramdisk: ===\n\n" >> $outfile

# setup spdk
cd $spdk_dir
# ./scripts/pkgdep.sh
# ./configure --with-rbd
# make -j10
sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'

# start server
mkdir -p /tmp/lsvd_rcache /tmp/lsvd_wcache

set +e
scripts/rpc.py spdk_kill_instance SIGTERM
pkill nvmf_tgt
set -e

./build/bin/nvmf_tgt &

# == setup spdk target ===

scripts/rpc.py bdev_malloc_create -b Malloc0 1024 512

# create subsystem
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0

# setup listener
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 10.1.0.5 -s 9922

# == run benchmark and collect results ===
cd $experiment_dir
ssh root@$client_host -i /root/.ssh/id_rsa "bash -s" < client-bench.bash >> $outfile 2>&1

# cleanup
cd $spdk_dir
scripts/rpc.py bdev_malloc_delete Malloc0
scripts/rpc.py spdk_kill_instance SIGTERM
