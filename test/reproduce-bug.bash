#!/usr/bin/env bash

lsvd_dir=$(git rev-parse --show-toplevel)

set -xeuo pipefail
ulimit -c unlimited
ulimit -f unlimited

source $lsvd_dir/experiments/common.bash

cd $lsvd_dir
make clean
make -j$(nproc) release

kill_nvmf

pool_name=rssd2
imgname=lsvd-benchmark

launch_lsvd_gw_background /mnt/nvme /mnt/nvme-remote/ $((240 * 1024 * 1024 * 1024))
configure_nvmf_common 10.1.0.5
add_rbd_img $pool_name $imgname
trap "cleanup_nvmf_rbd bdev_$imgname; cleanup_nvmf; exit" SIGINT SIGTERM EXIT

lsvd_pid=$(ps aux | perl -lane 'print @F[1] if /nvmf_tgt/ and not /perl/')
gdb attach $lsvd_pid -ex cont

wait
