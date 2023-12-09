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

pool_name=$1

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10.1.[0-9.]+)\/24/')
client_ip=${client_ip:-10.1.0.6}
rcache=/mnt/nvme/
wlog=/mnt/nvme-remote/

outfile=$lsvd_dir/debug.out
truncate -s 0 $outfile

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=lsvd-debug
imgsize=80g
blocksize=4096

source $lsvd_dir/experiments/common.bash

# Build LSVD
echo '===Building LSVD...'
cd $lsvd_dir
# make clean
make -j20 nosan

create_lsvd_thin $pool_name $imgname $imgsize

kill_nvmf

export LSVD_NO_GC=1
launch_lsvd_gw_background $rcache $wlog $((5 * 1024 * 1024 * 1024))

configure_nvmf_common $gw_ip
exit
add_rbd_img $pool_name $imgname
# trap "cleanup_nvmf_rbd bdev_$imgname; cleanup_nvmf; exit" SIGINT SIGTERM EXIT

# attach gdb
lsvd_pid=$(ps aux | perl -lane 'print @F[1] if /nvmf_tgt/ and not /perl/')
gdb attach $lsvd_pid -ex cont
# gdb attach $lsvd_pid
wait
