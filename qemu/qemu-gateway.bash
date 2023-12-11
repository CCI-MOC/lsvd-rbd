#!/usr/bin/env bash

set -xeuo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <pool_name> <imgname>"
  exit
fi

# pool must already exist
pool_name=$1
imgname=$2

lsvd_dir=$(git rev-parse --show-toplevel)
source $lsvd_dir/.env
source $lsvd_dir/experiments/common.bash

echo "Running gateway on $gw_ip, client on $client_ip"
echo "Running with image $pool_name/$imgname"

cd $lsvd_dir
make clean
make -j20 release

# make sure image exists
rados -p $pool_name stat $imgname

kill_nvmf
launch_lsvd_gw_background $rcache $wlog $((5 * 1024 * 1024 * 1024))
configure_nvmf_common $gw_ip
add_rbd_img $pool_name $imgname

wait
