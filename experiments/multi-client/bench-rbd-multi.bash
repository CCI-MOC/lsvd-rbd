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
outfile=$lsvd_dir/experiments/results/$cur_time.rbd-multi.$pool_name.txt

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=rbd-benchmark
imgsize=5G
blocksize=4096

source $lsvd_dir/tools/utils.bash

rbd -p $pool_name rm $imgname.multi.1 || true
rbd -p $pool_name rm $imgname.multi.2 || true
rbd -p $pool_name rm $imgname.multi.3 || true
rbd -p $pool_name rm $imgname.multi.4 || true
rbd -p $pool_name rm $imgname.multi.5 || true
rbd -p $pool_name rm $imgname.multi.6 || true
rbd -p $pool_name rm $imgname.multi.7 || true
rbd -p $pool_name rm $imgname.multi.8 || true

# rbd -p $pool_name create --size $imgsize $imgname.multi.1 &
# rbd -p $pool_name create --size $imgsize $imgname.multi.2 &
# rbd -p $pool_name create --size $imgsize $imgname.multi.3 &
# rbd -p $pool_name create --size $imgsize $imgname.multi.4 &

rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multi.1 &
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multi.2 &
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multi.3 &
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multi.4 &
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multi.5 &
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multi.6 &
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multi.7 &
rbd -p $pool_name create --size $imgsize --thick-provision $imgname.multi.8 &

wait

kill_nvmf

fstrim /mnt/nvme
launch_gw_background
configure_nvmf_common $gw_ip

add_rbd_img_new_cluster $pool_name $imgname.multi.1
add_rbd_img_new_cluster $pool_name $imgname.multi.2
add_rbd_img_new_cluster $pool_name $imgname.multi.3
add_rbd_img_new_cluster $pool_name $imgname.multi.4
add_rbd_img_new_cluster $pool_name $imgname.multi.5
add_rbd_img_new_cluster $pool_name $imgname.multi.6
add_rbd_img_new_cluster $pool_name $imgname.multi.7
add_rbd_img_new_cluster $pool_name $imgname.multi.8

trap "cleanup_nvmf; exit" SIGINT SIGTERM EXIT
run_client_bench $client_ip $outfile multi-client/client-bench-multi.bash "read_entire_img=0"
