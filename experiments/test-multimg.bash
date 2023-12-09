#!/usr/bin/env bash

set -xeuo pipefail

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root"
  exit
fi

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <pool_name> <imgname>"
  exit
fi

# pool must already exist
pool_name=$1
imgname=$2

cur_time=$(date +"%FT%T")

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10.1.[0-9.]+)\/24/')
client_ip=${client_ip:-10.1.0.6}
outfile=$lsvd_dir/experiments/results/$cur_time.lsvd.multimg.txt
rcache=/mnt/nvme/
wlog=/mnt/nvme-remote/

echo "Running gateway on $gw_ip, client on $client_ip"
echo "Running with image $pool_name/$imgname"

blocksize=4096

source $lsvd_dir/experiments/common.bash

# Build LSVD
echo '===Building LSVD...'
cd $lsvd_dir
make clean
make -j20 release

# make sure image exists
rados -p $pool_name stat $imgname

kill_nvmf
export LSVD_NO_GC=1
launch_lsvd_gw_background $rcache $wlog $((5 * 1024 * 1024 * 1024))

exit

scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1

# repeat these two lines for every image, they're added to different namespaces
# on the same controller
scripts/rpc.py bdev_rbd_create $pool_name $imgname $blocksize -c rbd_cluster -b $bdev_name
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev_name

scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a $gateway_ip -s 9922

wait

