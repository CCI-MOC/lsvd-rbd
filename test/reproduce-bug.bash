#!/usr/bin/env bash

lsvd_dir=$(git rev-parse --show-toplevel)

set -xeuo pipefail
ulimit -c unlimited
ulimit -f unlimited

source $lsvd_dir/experiments/common.bash

cd $lsvd_dir
make clean
# make -j$(nproc) release
make -j$(nproc) debug

kill_nvmf

pool_name=rssd2
imgname=lsvd-benchmark

echo 4096 >/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

cd $lsvd_dir/spdk
mkdir -p /mnt/nvme/lsvd-read/ /mnt/nvme-remote/lsvd-write/
export LSVD_RCACHE_DIR=/mnt/nvme/lsvd-read/
export LSVD_WCACHE_DIR=/mnt/nvme-remote/lsvd-write/
export LSVD_GC_THRESHOLD=40
export LSVD_CACHE_SIZE=$((240 * 1024 * 1024 * 1024)) # 240GiB

# clear out write log directory
rm -rf /mnt/nvme-remote/lsvd-write/*
LD_PRELOAD="/lib/x86_64-linux-gnu/libasan.so.8 /lib/x86_64-linux-gnu/libubsan.so.1 $lsvd_dir/liblsvd.so" \
	./build/bin/nvmf_tgt -m '[0,1,2,3]' &

sleep 5

configure_nvmf_common 10.1.0.5
add_rbd_img $pool_name $imgname
trap "cleanup_nvmf_rbd bdev_$imgname; cleanup_nvmf; exit" SIGINT SIGTERM EXIT

lsvd_pid=$(ps aux | perl -lane 'print @F[1] if /nvmf_tgt/ and not /perl/')
gdb spdk/build/bin/nvmf_tgt $lsvd_pid -ex cont

wait
