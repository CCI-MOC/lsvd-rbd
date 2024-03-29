#!/usr/bin/env bash

set -xeuo pipefail
ulimit -c

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <pool_name> <configuration>"
  exit
fi

# config variables
pool_name=$1
configuration=$2
cur_time=$(date +"%FT%T")

lsvd_dir=$(git rev-parse --show-toplevel)
source $lsvd_dir/.env
source $lsvd_dir/tools/utils.bash
echo "Running gateway on $gw_ip, client on $client_ip"

outfile=$lsvd_dir/experiments/results/$cur_time.$configuration.$pool_name.txt

# check that pool exists
rados -p $pool_name ls

imgsize=80g
blocksize=4096

kill_nvmf

case $configuration in

"lsvd-20g")
	imgname=lsvd-benchmark
	lsvd_cache_size=$((20 * 1024 * 1024 * 1024))

	# Build LSVD
	cd $lsvd_dir
	make setup
	cd build-rel
	meson compile

	create_lsvd_thick $pool_name $imgname $imgsize

	fstrim /mnt/nvme
	launch_lsvd_gw_background $rcache $wlog $lsvd_cache_size
	configure_nvmf_common $gw_ip
	add_rbd_img $pool_name $imgname
	trap "cleanup_nvmf_rbd bdev_$imgname; cleanup_nvmf; exit" SIGINT SIGTERM EXIT

	run_client_bench $client_ip $outfile client-bench.bash "read_entire_img=1"
	;;

"lsvd-240g")
	imgname=lsvd-benchmark
	lsvd_cache_size=$((240 * 1024 * 1024 * 1024))

	create_lsvd_thick $pool_name $imgname $imgsize

	fstrim /mnt/nvme
	launch_lsvd_gw_background $rcache $wlog $lsvd_cache_size
	configure_nvmf_common $gw_ip
	add_rbd_img $pool_name $imgname
	trap "cleanup_nvmf_rbd bdev_$imgname; cleanup_nvmf; exit" SIGINT SIGTERM EXIT

	run_client_bench $client_ip $outfile client-bench.bash "read_entire_img=1"
	;;

"rbd")
	imgname=rbd-benchmark

	rbd -p $pool_name rm $imgname || true
	rbd -p $pool_name create --size $imgsize --thick-provision $imgname

	launch_gw_background
	configure_nvmf_common $gw_ip
	add_rbd_img $pool_name $imgname

	run_client_bench $client_ip $outfile
	cleanup_nvmf

	rbd -p $pool_name rm $imgname || true
	;;

"ramdisk")
	imgsize=81920000 # in kb (kib? unsure)

	rmmod brd || true
	modprobe brd rd_size=$imgsize max_part=1 rd_nr=1

	launch_gw_background
	configure_nvmf_common $gw_ip

	# liburing issues, use aio for now
	# scripts/rpc.py bdev_uring_create /dev/ram0 bdev_uring0 $blocksize
	scripts/rpc.py bdev_aio_create /dev/ram0 bdev_uring0 $blocksize
	scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 bdev_uring0

	run_client_bench $client_ip $outfile
	cleanup_nvmf

	rmmod brd
	;;

"nvme")
	if [[ $# -lt 3 ]]; then
		echo "Usage: $0 <pool_name> nvme <device_name>"
		exit
	fi

	launch_gw_background

	configure_nvmf_common $gw_ip

	# liburing issues, use aio for now
	# scripts/rpc.py bdev_uring_create /dev/ram0 bdev_uring0 $blocksize
	scripts/rpc.py bdev_aio_create $dev_name bdev_uring0 $blocksize
	scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 bdev_uring0

	run_client_bench $client_ip $outfile
	cleanup_nvmf
	;;

"nvme-multi")
	fstrim /mnt/nvme

	dd if=/dev/random of=/mnt/nvme/$imgname.multi.1 bs=1M count=20480 status=progress &
	dd if=/dev/random of=/mnt/nvme/$imgname.multi.2 bs=1M count=20480 status=progress &
	dd if=/dev/random of=/mnt/nvme/$imgname.multi.3 bs=1M count=20480 status=progress &
	dd if=/dev/random of=/mnt/nvme/$imgname.multi.4 bs=1M count=20480 status=progress &

	wait

	kill_nvmf

	launch_gw_background
	configure_nvmf_common $gw_ip

	scripts/rpc.py bdev_aio_create /mnt/nvme/$imgname.multi.1 f1 4096
	scripts/rpc.py bdev_aio_create /mnt/nvme/$imgname.multi.2 f2 4096
	scripts/rpc.py bdev_aio_create /mnt/nvme/$imgname.multi.3 f3 4096
	scripts/rpc.py bdev_aio_create /mnt/nvme/$imgname.multi.4 f4 4096

	scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 f1
	scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 f2
	scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 f3
	scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 f4

	trap "cleanup_nvmf; exit" SIGINT SIGTERM EXIT
	run_client_bench $client_ip $outfile client-bench-multi.bash "read_entire_img=0"
	;;

*)
	echo "Invalid configuration $configuration"
	exit 1
	;;

esac
