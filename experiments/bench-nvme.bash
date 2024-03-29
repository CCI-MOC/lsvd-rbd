#!/usr/bin/env bash

set -xeuo pipefail

if [ "$EUID" -ne 0 ]
  then echo "Please run as root"
  exit
fi

cur_time=$(date +"%FT%T")

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10\.1\.[0-9.]+)\/24/' | head -n 1)
client_ip=${client_ip:-10.1.0.6}
outfile=$lsvd_dir/experiments/results/$cur_time.nvme.txt
dev_name=$1

echo "Running gateway on $gw_ip, client on $client_ip"
echo "Using device $dev_name"

imgsize=81920000 # in kb (kib? unsure)
blocksize=4096

source $lsvd_dir/experiments/common.bash

kill_nvmf
launch_gw_background

configure_nvmf_common $gw_ip

# liburing issues, use aio for now
# scripts/rpc.py bdev_uring_create /dev/ram0 bdev_uring0 $blocksize
scripts/rpc.py bdev_aio_create $dev_name bdev_uring0 $blocksize
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 bdev_uring0

run_client_bench $client_ip $outfile
cleanup_nvmf

rmmod brd
