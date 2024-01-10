#!/usr/bin/env bash

set -xeuo pipefail
ulimit -c

if [ -z "${1:-}" ]; then
  echo "Please provide a pool name"
  exit
fi

# pool must already exist
pool_name=$1
lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10\.1\.[0-9.]+)\/24/' | head -n 1)
client_ip=${client_ip:-10.1.0.6}

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=lsvd-benchmark
imgsize=10g
blocksize=4096

source $lsvd_dir/experiments/common.bash

cd $lsvd_dir
make clean
make -j20 release

create_lsvd_thick $pool_name $imgname.multigw.5 $imgsize &
create_lsvd_thick $pool_name $imgname.multigw.6 $imgsize &
create_lsvd_thick $pool_name $imgname.multigw.7 $imgsize &
create_lsvd_thick $pool_name $imgname.multigw.8 $imgsize &
wait

kill_nvmf

launch_lsvd_gw_background /mnt/nvme /mnt/nvme-remote $((120 * 1024 * 1024 * 1024)))
configure_nvmf_common $gw_ip

add_rbd_img $pool_name $imgname.multigw.5
add_rbd_img $pool_name $imgname.multigw.6
add_rbd_img $pool_name $imgname.multigw.7
add_rbd_img $pool_name $imgname.multigw.8

trap "cleanup_nvmf; exit" SIGINT SIGTERM EXIT
wait
