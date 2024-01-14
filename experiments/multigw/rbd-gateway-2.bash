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

rbd -p $pool_name rm $imgname.multigw.5 || true
rbd -p $pool_name rm $imgname.multigw.6 || true
rbd -p $pool_name rm $imgname.multigw.7 || true
rbd -p $pool_name rm $imgname.multigw.8 || true
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multigw.5
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multigw.6
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multigw.7
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multigw.8
wait

kill_nvmf

launch_lsvd_gw_background /mnt/nvme /mnt/nvme-malloc $((120 * 1024 * 1024 * 1024))

scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:lsvd-gw2 -a -s SPDKMULTIGW0000002 -d SPDK_LSVD_GW2
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:lsvd-gw2 -t tcp -a $gw_ip -s 9922

function add_rbd_img {
  cd $lsvd_dir/spdk
  local pool=$1
  local img=$2
  local bdev="bdev_$img"
  scripts/rpc.py bdev_rbd_create $pool $img 4096 -c rbd_cluster -b $bdev
  scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:lsvd-gw2 $bdev
}

add_rbd_img $pool_name $imgname.multigw.5
add_rbd_img $pool_name $imgname.multigw.6
add_rbd_img $pool_name $imgname.multigw.7
add_rbd_img $pool_name $imgname.multigw.8

trap "cleanup_nvmf; exit" SIGINT SIGTERM EXIT
wait
