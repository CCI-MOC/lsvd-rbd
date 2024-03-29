#!/usr/bin/env bash

set -euo pipefail

for pair in $*; do
	if [ ${pair#*=} != $pair ]; then
		eval $pair
	else
		echo ERROR: $pair not an assignment
	fi
done

printf "===Starting client benchmark\n\n"

trap 'umount /mnt/fsbench || true; nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true; exit' SIGINT SIGTERM SIGHUP EXIT

modprobe nvme-fabrics
nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true

gw_ip=${gw_ip:-10.1.0.5}
nvme connect -t tcp --traddr $gw_ip -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 5

nvme list
# nvme connect sometimes gives the wrong name (you had one job >_<)
lsvd_devs=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $lsvd_devs\n"

# === run the benchmarks ===

num_fio_processes=4
# num_fio_processes=1
fio_size="20GB"

read_entire_img=${read_entire_img:-0}

# warm the cache by reading in the entire image
if [[ $read_entire_img -eq 1 ]]; then
	printf "\n\n===Reading entire image to warm cache===\n\n"
	for dev_name in $lsvd_devs; do
		dd if=$dev_name of=/dev/null bs=1048576 count=20479 status=progress &
	done
	wait
fi

# fio

function run_fio_1 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, disks=1 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based --randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (disks=1, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

function run_fio_2 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, disks=2 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based --randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		--name=j2 --filename=/dev/nvme1n2 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (disks=2, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

function run_fio_3 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, disks=3 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based --randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		--name=j2 --filename=/dev/nvme1n2 \
		--name=j3 --filename=/dev/nvme1n3 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (disks=3, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

function run_fio_4 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, disks=4 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based --randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		--name=j2 --filename=/dev/nvme1n2 \
		--name=j3 --filename=/dev/nvme1n3 \
		--name=j4 --filename=/dev/nvme1n4 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (disks=4, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

function run_fio_5 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, disks=5 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based --randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		--name=j2 --filename=/dev/nvme1n2 \
		--name=j3 --filename=/dev/nvme1n3 \
		--name=j4 --filename=/dev/nvme1n4 \
		--name=j4 --filename=/dev/nvme1n5 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (disks=5, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

function run_fio_6 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, disks=6 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based --randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		--name=j2 --filename=/dev/nvme1n2 \
		--name=j3 --filename=/dev/nvme1n3 \
		--name=j4 --filename=/dev/nvme1n4 \
		--name=j4 --filename=/dev/nvme1n5 \
		--name=j4 --filename=/dev/nvme1n6 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (disks=6, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

function run_fio_7 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, disks=7 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based --randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		--name=j2 --filename=/dev/nvme1n2 \
		--name=j3 --filename=/dev/nvme1n3 \
		--name=j4 --filename=/dev/nvme1n4 \
		--name=j4 --filename=/dev/nvme1n5 \
		--name=j4 --filename=/dev/nvme1n6 \
		--name=j4 --filename=/dev/nvme1n7 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (disks=7, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

function run_fio_8 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, disks=8 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based --randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		--name=j2 --filename=/dev/nvme1n2 \
		--name=j3 --filename=/dev/nvme1n3 \
		--name=j4 --filename=/dev/nvme1n4 \
		--name=j4 --filename=/dev/nvme1n5 \
		--name=j4 --filename=/dev/nvme1n6 \
		--name=j4 --filename=/dev/nvme1n7 \
		--name=j4 --filename=/dev/nvme1n8 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (disks=8, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

# warmup? not sure why this is needed
run_fio_4 randread 60 256 4ki

# test scaling
run_fio_1 randread 60 128 4ki
run_fio_2 randread 60 128 4ki
run_fio_3 randread 60 128 4ki
run_fio_4 randread 60 128 4ki
run_fio_5 randread 60 128 4ki
run_fio_6 randread 60 128 4ki
run_fio_7 randread 60 128 4ki
run_fio_8 randread 60 128 4ki

# run_fio_1 read 60 128 4ki
# run_fio_2 read 60 128 4ki
# run_fio_3 read 60 128 4ki
# run_fio_4 read 60 128 4ki

# # test queue depth
# run_fio_4 randread 60 1 4k
# run_fio_4 randread 60 32 4k
# run_fio_4 randread 60 64 4k
# run_fio_4 randread 60 128 4k
# run_fio_4 read 60 1 4k
# run_fio_4 read 60 32 4k
# run_fio_4 read 60 64 4k
# run_fio_4 read 60 128 4k

# # test block sizes (are we still iops limited?)
# run_fio_4 randread 60 128 4ki
# run_fio_4 randread 60 128 8ki
# run_fio_4 randread 60 128 16ki
# run_fio_4 randread 60 128 32ki
# run_fio_4 randread 60 128 64ki
# run_fio_4 read 60 128 4ki
# run_fio_4 read 60 128 8ki
# run_fio_4 read 60 128 16ki
# run_fio_4 read 60 128 32ki
# run_fio_4 read 60 128 64ki
