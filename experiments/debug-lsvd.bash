#!/usr/bin/env bash

set -xeuo pipefail

if [ -z "${1:-}" ]
  then echo "Please provide a pool name"
  exit
fi

pool_name=$1
imgname=lsvd-debug
imgsize=80g

lsvd_dir=$(git rev-parse --show-toplevel)
source $lsvd_dir/.env
source $lsvd_dir/experiments/common.bash

echo "Running gateway on $gw_ip, client on $client_ip"

cd $lsvd_dir
make clean
make -j20 nosan
# make -j20 release

create_lsvd_thick $pool_name $imgname $imgsize
rados -p $pool_name stat $imgname

kill_nvmf

# export LSVD_NO_GC=1
launch_lsvd_gw_background $rcache $wlog $((20 * 1024 * 1024 * 1024))

configure_nvmf_common $gw_ip
add_rbd_img $pool_name $imgname
# trap "cleanup_nvmf_rbd bdev_$imgname; cleanup_nvmf; exit" SIGINT SIGTERM EXIT

# attach gdb
lsvd_pid=$(ps aux | perl -lane 'print @F[1] if /nvmf_tgt/ and not /perl/')
gdb attach $lsvd_pid -ex cont
# gdb attach $lsvd_pid
wait
