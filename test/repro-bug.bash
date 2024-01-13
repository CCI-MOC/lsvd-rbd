#!/usr/bin/env bash

set -xeuo pipefail
ulimit -c unlimited

export lsvd_cache_size=$((240 * 1024 * 1024 * 1024))

# pool must already exist
pool_name=rssd2
cur_time=$(date +"%FT%T")
default_cache_size=$((20 * 1024 * 1024 * 1024))
cache_size=${lsvd_cache_size:-$default_cache_size}

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10\.1\.[0-9.]+)\/24/' | head -n 1)
client_ip=${client_ip:-10.1.0.6}
rcache=/mnt/nvme/
wlog=/mnt/nvme-remote/
cache_size_gb=$(($cache_size / 1024 / 1024 / 1024))
outfile=$lsvd_dir/experiments/results/debug.out

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=lsvd-bug-repro
imgsize=80g
blocksize=4096

source $lsvd_dir/experiments/common.bash

# Build LSVD
echo '===Building LSVD...'
cd $lsvd_dir
make clean
# make -j20 release
make -j20 debug

# create_lsvd_thin $pool_name $imgname $imgsize
# create_lsvd_thick $pool_name $imgname $imgsize

kill_nvmf

fstrim /mnt/nvme

mkdir -p /mnt/nvme//lsvd-read/ /mnt/nvme-remote//lsvd-write/
export LSVD_RCACHE_DIR=/mnt/nvme//lsvd-read/
export LSVD_WCACHE_DIR=/mnt/nvme-remote//lsvd-write/
export LSVD_GC_THRESHOLD=40
export LSVD_CACHE_SIZE=$lsvd_cache_size
rm -rf /mnt/nvme-remote/lsvd-write/*

LD_PRELOAD="/lib/x86_64-linux-gnu/libasan.so.8 /lib/x86_64-linux-gnu/libubsan.so.1 /home/isaackhor/code/lsvd-rbd/liblsvd.so" \
	./build/bin/nvmf_tgt -m '[0,1,2,3]' &

sleep 5

configure_nvmf_common $gw_ip
add_rbd_img $pool_name $imgname
trap "cleanup_nvmf_rbd bdev_$imgname; cleanup_nvmf; exit" SIGINT SIGTERM EXIT

lsvd_pid=$(ps aux | perl -lane 'print @F[1] if /nvmf_tgt/ and not /perl/')
gdb attach $lsvd_pid -ex cont

exit

cd $lsvd_dir
ssh $client_ip 'mkdir -p /tmp/filebench; rm -rf /tmp/filebench/*'
scp ./experiments/filebench-workloads/*.f root@$client_ip:/tmp/filebench/
ssh $client_ip "bash -s gw_ip=$gw_ip" <./test/repro-bug-client.bash 2>&1 | tee -a $outfile
