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
cache_dir=/mnt/nvme/lsvd-cache/
outfile=$lsvd_dir/experiments/results/$cur_time.lsvd.txt

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=lsvd-qemu-test
blocksize=4096

source $lsvd_dir/experiments/common.bash

# Build LSVD
echo '===Building LSVD...'
cd $lsvd_dir
make clean
make -j20 release

# make sure image exists
rados -p $pool_name stat $imgname

kill_nvmf
launch_lsvd_gw_background $cache_dir
configure_nvmf_rbd $pool_name $imgname $blocksize bdev_lsvd0
configure_nvmf_transport $gw_ip bdev_lsvd0

wait
