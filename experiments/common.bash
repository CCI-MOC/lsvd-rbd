#!/usr/bin/env bash

set -xeuo pipefail

function kill_nvmf {
	cd $lsvd_dir/spdk
	scripts/rpc.py spdk_kill_instance SIGTERM >/dev/null || true
	scripts/rpc.py spdk_kill_instance SIGKILL >/dev/null || true
	pkill -f nvmf_tgt || true
	pkill -f reactor_0 || true
	sleep 5
}

function configure_nvmf_rbd {
	local pool_name=$1
	local imgname=$2
	local blocksize=$3
	local bdev_name=$4

	cd $lsvd_dir/spdk
	scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
	scripts/rpc.py bdev_rbd_create $pool_name $imgname $blocksize -c rbd_cluster -b $bdev_name
}

function configure_nvmf_transport {
	local gateway_ip=$1
	local bdev_name=$2

	cd $lsvd_dir/spdk
	scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
	scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev_name
	scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
	scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a $gateway_ip -s 9922
}

function launch_lsvd_gw_background {
	local cache_parent_dir=$1

	cd $lsvd_dir/spdk
	mkdir -p $cache_parent_dir/{read,write}
	export LSVD_RCACHE_DIR=$cache_parent_dir/read/
	export LSVD_WCACHE_DIR=$cache_parent_dir/write/
	export LSVD_GC_THRESHOLD=40
	export LSVD_CACHE_SIZE=5368709120 # 5GiB
	LD_PRELOAD=$lsvd_dir/liblsvd.so ./build/bin/nvmf_tgt &

	sleep 5
}

function launch_gw_background {
	cd $lsvd_dir/spdk
	./build/bin/nvmf_tgt &

	sleep 5
}

function cleanup_nvmf_rbd {
	local bdev_name=$1

	cd $lsvd_dir/spdk
	scripts/rpc.py bdev_rbd_delete $bdev_name
	scripts/rpc.py bdev_rbd_unregister_cluster rbd_cluster
}

function cleanup_nvmf {
	cd $lsvd_dir/spdk
	scripts/rpc.py spdk_kill_instance SIGTERM
}

function attach_gdb_lsvd {
	lsvd_pid=$(ps aux | perl -lane 'print @F[1] if /nvmf_tgt/ and not /perl/')
	gdb attach $lsvd_pid -ex cont
}

function create_lsvd_thin {
	local pool=$1
	local img=$2
	local size=$3

	cd $lsvd_dir
	# ./imgtool --delete --rados $pool/$img || true
	./remove_objs.py $pool $img

	./imgtool --create --rados --size=$size $pool/$img

	# make sure image exists
	rados -p $pool stat $img
}

function create_lsvd_thick {
	local pool=$1
	local img=$2
	local size=$3

	cd $lsvd_dir
	./remove_objs.py $pool $img
	./thick-image --size=$size $pool/$img

	rados -p $pool stat $img
}

function run_client_bench {
	local client_ip=$1
	local outfile=$2

	cd $lsvd_dir/experiments
	ssh $client_ip 'mkdir -p /tmp/filebench; rm -rf /tmp/filebench/*'
	scp ./filebench-workloads/*.f root@$client_ip:/tmp/filebench/
	# ssh $client_ip "bash -s gw_ip=$gw_ip" < client-bench.bash 2>&1 | tee -a $outfile
	ssh $client_ip "bash -s gw_ip=$gw_ip" < client-bench-abbrev.bash 2>&1 | tee -a $outfile

	perl -lane 'print if s/^RESULT: //' $outfile | tee -a $outfile
}
