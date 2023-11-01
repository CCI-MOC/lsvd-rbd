#!/usr/bin/env bash

set -xeuo pipefail

if [ "$EUID" -ne 0 ]
  then echo "Please run as root"
  exit
fi

pool_name="triple-ssd"
lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10.1.[0-9.]+)\/24/')
client_ip=${client_ip:-10.1.0.6}
cache_dir=/mnt/nvme/lsvd-cache/

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=lsvd-qemu-ubuntu2204
imgsize=5g
blocksize=4096

source $lsvd_dir/experiments/common.bash

cd $lsvd_dir
make clean
make nosan -j20

# make sure image exists
rados -p $pool_name stat $imgname

# Create the image
# ./imgtool --delete --rados $pool_name/$imgname || true
# ./imgtool --create --rados --size=5g $pool_name/$imgname

cd $lsvd_dir/spdk
kill_lsvd_nvmf
launch_lsvd_gw_background $cache_dir
setup_nvmf_target $pool_name $imgname $blocksize

wait
