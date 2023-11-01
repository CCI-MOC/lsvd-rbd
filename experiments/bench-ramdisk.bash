#!/usr/bin/env bash

set -xeuo pipefail

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

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10.1.[0-9.]+)\/24/')
client_ip=${client_ip:-10.1.0.6}
outfile=$lsvd_dir/experiments/results/$cur_time.ramdisk.txt

echo "Running gateway on $gw_ip, client on $client_ip"

imgsize=40960000 # in kb (kib? unsure)
blocksize=4096

source $lsvd_dir/experiments/common.bash

# create ramdisk
rmmod brd || true
modprobe brd rd_size=$imgsize max_part=1 rd_nr=1

kill_nvmf
launch_gw_background
scripts/rpc.py bdev_uring_create /dev/ram0 bdev_uring0 $blocksize
configure_nvmf_uring /dev/ram0 4096
configure_nvmf_transport $gw_ip

run_client_bench $client_ip $outfile
cleanup_nvmf

rmmod brd
