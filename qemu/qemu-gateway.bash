#!/usr/bin/env bash

set -xeuo pipefail

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root"
  exit
fi

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <pool_name> <imgname>"
  exit
fi

# pool must already exist
pool_name=$1
imgname=$2

cur_time=$(date +"%FT%T")

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10.1.[0-9.]+)\/24/')
client_ip=${client_ip:-10.1.0.6}
cache_dir=/mnt/nvme/lsvd-cache/
outfile=$lsvd_dir/experiments/results/$cur_time.lsvd.txt

echo "Running gateway on $gw_ip, client on $client_ip"
echo "Running with image $pool_name/$imgname"

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
configure_nvmf_common
add_rbd_img $pool_name $imgname

wait
