#!/usr/bin/env bash

set -xeuo pipefail
ulimit -c

if [ -z "${1:-}" ]; then
  echo "Please provide a pool name"
  exit
fi

# pool must already exist
pool_name=$1
cur_time=$(date +"%FT%T")
default_cache_size=$((120 * 1024 * 1024 * 1024))
cache_size=${lsvd_cache_size:-$default_cache_size}

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10\.1\.[0-9.]+)\/24/' | head -n 1)
client_ip=${client_ip:-10.1.0.6}
rcache=/mnt/nvme/
wlog=/mnt/nvme-remote/
cache_size_gb=$(($cache_size / 1024 / 1024 / 1024))
outfile=$lsvd_dir/experiments/results/$cur_time.lsvd-multi.$pool_name.txt

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=lsvd-benchmark
imgsize=5g
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

create_lsvd_thick $pool_name $imgname.multi.1 $imgsize &
create_lsvd_thick $pool_name $imgname.multi.2 $imgsize &
create_lsvd_thick $pool_name $imgname.multi.3 $imgsize &
create_lsvd_thick $pool_name $imgname.multi.4 $imgsize &
create_lsvd_thick $pool_name $imgname.multi.5 $imgsize &
create_lsvd_thick $pool_name $imgname.multi.6 $imgsize &
create_lsvd_thick $pool_name $imgname.multi.7 $imgsize &
create_lsvd_thick $pool_name $imgname.multi.8 $imgsize &

wait

kill_nvmf

fstrim /mnt/nvme
launch_lsvd_gw_background $rcache $wlog $cache_size
configure_nvmf_common $gw_ip

add_rbd_img $pool_name $imgname.multi.1
add_rbd_img $pool_name $imgname.multi.2
add_rbd_img $pool_name $imgname.multi.3
add_rbd_img $pool_name $imgname.multi.4
add_rbd_img $pool_name $imgname.multi.5
add_rbd_img $pool_name $imgname.multi.6
add_rbd_img $pool_name $imgname.multi.7
add_rbd_img $pool_name $imgname.multi.8

trap "cleanup_nvmf; exit" SIGINT SIGTERM EXIT
run_client_bench $client_ip $outfile multi-client/client-bench-multi.bash "read_entire_img=1"
