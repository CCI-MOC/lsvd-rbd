#!/usr/bin/env bash

set -xeuo pipefail

for pair in $*; do
	if [ ${pair#*=} != $pair ]; then
		eval $pair
	else
		echo ERROR: $pair not an assignment
	fi
done

printf "===Starting client benchmark\n\n"

trap 'umount /mnt/fsbench || true; nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true; exit' SIGINT SIGTERM SIGHUP EXIT

# if [ "$EUID" -ne 0 ]
#   then echo "Please run as root"
#   exit
# fi

modprobe nvme-fabrics
nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true
nvme disconnect -n nqn.2016-06.io.spdk:cnode3 || true

gw_ip=${gw_ip:-10.1.0.5}
nvme connect -t tcp --traddr $gw_ip -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 5

nvme list
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $dev_name\n"

# === run the benchmarks ===

read_entire_img=${read_entire_img:-0}

# warm the cache by reading in the entire image
# TODO this is a temporary workaround; figure out a better way to warm the cache
# for fio later
if [[ $read_entire_img -eq 1 ]]; then
	printf "\n\n===Reading entire image to warm cache===\n\n"
	# dd if=$dev_name of=/dev/null bs=1048576 status=progress
	dd if=$dev_name of=/dev/null bs=1048576 status=progress
fi

set +x

# fio

function run_fio {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4===\n\n"
	fio \
		--name=global \
		--rw=$1 \
		--runtime=$2 \
		--iodepth=$3 \
		--bs=$4 \
		--ioengine=io_uring \
		--time_based \
		--randseed=42 \
		--size=10g \
		--eta-newline=1 \
		--direct=1 \
		--group_reporting \
		--name=j1 --filename=$dev_name \
		| tee /tmp/client-bench-results.txt

	printf "\nRESULT: Fio (iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt

	sleep 2
}

run_fio randread 30 1 4ki
run_fio randread 30 8 4ki
run_fio randread 30 16 4ki
run_fio randread 30 24 4ki
run_fio randread 30 32 4ki
run_fio randread 30 40 4ki
run_fio randread 30 48 4ki
run_fio randread 30 56 4ki
run_fio randread 30 64 4ki
run_fio randread 30 80 4ki
run_fio randread 30 96 4ki
run_fio randread 30 112 4ki
run_fio randread 30 128 4ki
run_fio randread 30 160 4ki
run_fio randread 30 192 4ki
run_fio randread 30 224 4ki
run_fio randread 30 256 4ki
run_fio randread 30 320 4ki
run_fio randread 30 384 4ki
run_fio randread 30 448 4ki
run_fio randread 30 512 4ki
run_fio randread 30 640 4ki
run_fio randread 30 768 4ki
run_fio randread 30 896 4ki
run_fio randread 30 1024 4ki
