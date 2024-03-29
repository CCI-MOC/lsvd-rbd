#!/usr/bin/env bash

set -xeuo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <pool_name> <base imgname>"
  exit
fi

# pool must already exist
pool_name=$1
basename=$2

lsvd_dir=$(git rev-parse --show-toplevel)
source $lsvd_dir/.env
source $lsvd_dir/tools/utils.bash

echo "Running gateway on $gw_ip, client on $client_ip"
echo "Running with image $pool_name/$basename as the base"

cd $lsvd_dir
# make clean
make -j20 release

# make sure image exists
rados -p $pool_name stat $basename

./tools/remove_objs.py $pool_name clonetest
python3 tools/clone.py --rados $pool_name/$basename $pool_name/clonetest-1
python3 tools/clone.py --rados $pool_name/$basename $pool_name/clonetest-2
python3 tools/clone.py --rados $pool_name/$basename $pool_name/clonetest-3

kill_nvmf
export LSVD_NO_GC=1
launch_lsvd_gw_background $rcache $wlog $((5 * 1024 * 1024 * 1024))
configure_nvmf_common $gw_ip
add_rbd_img $pool_name clonetest-1
add_rbd_img $pool_name clonetest-2
add_rbd_img $pool_name clonetest-3

wait
