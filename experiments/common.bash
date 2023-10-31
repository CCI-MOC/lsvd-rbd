#!/usr/bin/env bash

set -xeuo pipefail

function kill_lsvd_nvmf {
	cd $lsvd_dir/spdk
	scripts/rpc.py spdk_kill_instance SIGTERM > /dev/null || true
	scripts/rpc.py spdk_kill_instance SIGKILL > /dev/null || true
	pkill -f nvmf_tgt || true
	pkill -f reactor_0 || true
	sleep 5
}

function setup_nvmf_target {
	cd $lsvd_dir/spdk
	scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
	scripts/rpc.py bdev_rbd_create $pool_name $imgname $blocksize -c rbd_cluster
	scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
	scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Ceph0
	scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
	scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a $gw_ip -s 9922
}

function launch_lsvd_gw_background {
	cd $lsvd_dir/spdk
	mkdir -p /mnt/nvme/lsvd-cache/{read,write}
	export LSVD_RCACHE_DIR=/mnt/nvme/lsvd-rcache
	export LSVD_WCACHE_DIR=/mnt/nvme/lsvd-wcache
	export LSVD_GC_THRESHOLD=40
	LD_PRELOAD=$lsvd_dir/liblsvd.so ./build/bin/nvmf_tgt &
}
