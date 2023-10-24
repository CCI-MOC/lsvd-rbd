#!/usr/bin/env bash
# run me as root

set -euo pipefail
set -x

pool_name=triple-ssd
imgname=lsvd-debug
blocksize=4096
cd /home/isaackhor/code/spdk/
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10.1.[0-9.]+)\/24/')
scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
scripts/rpc.py bdev_rbd_create $pool_name $pool_name/$imgname $blocksize -c rbd_cluster
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Ceph0
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a $gw_ip -s 9922
