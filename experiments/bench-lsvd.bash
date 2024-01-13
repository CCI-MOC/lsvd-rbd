#!/usr/bin/env bash

set -xeuo pipefail
ulimit -c

if [ -z "${1:-}" ]; then
  echo "Please provide a pool name"
  exit
fi

# pool must already exist
pool_name=$1
out_post=${2:-"std"}
cur_time=$(date +"%FT%T")
default_cache_size=$((20 * 1024 * 1024 * 1024))
cache_size=${lsvd_cache_size:-$default_cache_size}

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10\.1\.[0-9.]+)\/24/' | head -n 1)
client_ip=${client_ip:-10.1.0.6}
rcache=/mnt/nvme/
wlog=${lsvd_wlog_root:-/mnt/nvme-remote/}
cache_size_gb=$(($cache_size / 1024 / 1024 / 1024))
outfile=$lsvd_dir/experiments/results/$cur_time.lsvd-$out_post.$cache_size_gb.$pool_name.txt

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=lsvd-benchmark
imgsize=80g
blocksize=4096

source $lsvd_dir/experiments/common.bash

# Build LSVD
echo '===Building LSVD...'
cd $lsvd_dir
make clean
make -j20 release
# make -j20 nosan

# keep a copy of the library around to debug coredumps
mkdir -p $lsvd_dir/test/baklibs/
cp $lsvd_dir/liblsvd.so $lsvd_dir/test/baklibs/liblsvd.so.$cur_time

kill_nvmf

# create_lsvd_thin $pool_name $imgname $imgsize
create_lsvd_thick $pool_name $imgname $imgsize

fstrim /mnt/nvme
launch_lsvd_gw_background $rcache $wlog $cache_size
configure_nvmf_common $gw_ip
add_rbd_img $pool_name $imgname
trap "cleanup_nvmf_rbd bdev_$imgname; cleanup_nvmf; exit" SIGINT SIGTERM EXIT

run_client_bench $client_ip $outfile client-bench.bash "read_entire_img=1"