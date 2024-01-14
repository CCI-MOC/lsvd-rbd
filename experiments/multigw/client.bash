#!/usr/bin/env bash

set -xeuo pipefail

printf "===Starting client benchmark\n\n"

trap 'umount /mnt/fsbench || true; nvme disconnect -n nqn.2016-06.io.spdk:lsvd-gw1 || true; nvme disconnect -n nqn.2016-06.io.spdk:lsvd-gw2 || true; exit' SIGINT SIGTERM SIGHUP EXIT

modprobe nvme-fabrics
nvme disconnect -n nqn.2016-06.io.spdk:lsvd-gw1 || true
nvme disconnect -n nqn.2016-06.io.spdk:lsvd-gw2 || true

# 2 gateways
nvme connect -t tcp --traddr 10.1.0.5 -s 9922 -n nqn.2016-06.io.spdk:lsvd-gw1 -o normal
nvme connect -t tcp --traddr 10.1.0.8 -s 9922 -n nqn.2016-06.io.spdk:lsvd-gw2 -o normal
sleep 3

nvme list
# nvme connect sometimes gives the wrong name (you had one job >_<)
lsvd_devs=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $lsvd_devs\n"

# === run the benchmarks ===

read_entire_img=${read_entire_img:-0}
# read_entire_img=1
if [[ $read_entire_img -eq 1 ]]; then
	printf "\n\n===Reading entire image to warm cache===\n\n"
	for dev_name in $lsvd_devs; do
		dd if=$dev_name of=/dev/null bs=10485755 count=20479 &
	done
	wait
fi

set +x

# fio

function run_fio_4 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, gateways=1 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based \
		--randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		--name=j2 --filename=/dev/nvme1n2 \
		--name=j3 --filename=/dev/nvme1n3 \
		--name=j4 --filename=/dev/nvme1n4 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (gateways=1, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

function run_fio_8 {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, gateways=2 ===\n\n"
	fio \
		--name=global \
		--rw=$1 --runtime=$2 --iodepth=$3 --bs=$4 \
		--ioengine=io_uring --time_based \
		--randseed=42 --size=20g \
		--eta-newline=1 --direct=1 --group_reporting \
		--name=j1 --filename=/dev/nvme1n1 \
		--name=j2 --filename=/dev/nvme1n2 \
		--name=j3 --filename=/dev/nvme1n3 \
		--name=j4 --filename=/dev/nvme1n4 \
		--name=j5 --filename=/dev/nvme2n1 \
		--name=j6 --filename=/dev/nvme2n2 \
		--name=j7 --filename=/dev/nvme2n3 \
		--name=j8 --filename=/dev/nvme2n4 \
		| tee /tmp/client-bench-results.txt
	printf "\nRESULT: Fio (gateways=2, iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt
	sleep 2
}

# We run all the read workloads first, then the writes, to preserve the cache
# This is a hack; we need to figure out a better solution later

# warmup? not sure why this is needed
run_fio_4 randread 90 128 4ki
run_fio_8 randread 90 128 4ki

# test scaling
run_fio_4 randread 120 128 4ki
run_fio_8 randread 120 128 4ki
run_fio_4 read 120 128 4ki
run_fio_8 read 120 128 4ki

run_fio_4 randwrite 120 128 4ki
run_fio_8 randwrite 120 128 4ki
run_fio_4 write 120 128 4ki
run_fio_8 write 120 128 4ki

exit

# === disconnect and cleanup ===
# in the trap SIGTERM above
