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

# if [ "$EUID" -ne 0 ]
#   then echo "Please run as root"
#   exit
# fi

modprobe nvme-fabrics
nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true
nvme disconnect -n nqn.2016-06.io.spdk:cnode2 || true
nvme disconnect -n nqn.2016-06.io.spdk:cnode3 || true

gw_ip=${gw_ip:-10.1.0.5}
# see that it's there
# nvme discover -t tcp -a $gw_ip -s 9922
nvme connect -t tcp --traddr $gw_ip -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 5

nvme list
# nvme connect sometimes gives the wrong name (you had one job >_<)
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $dev_name\n"

# === run the benchmarks ===

#num_fio_processes=4
num_fio_processes=1
fio_size="80GiB"

read_entire_img=${read_entire_img:-0}

# BUG: limit to slightly under 80GiB, lsvd doesn't like the last few sectors
# don't thick provision here, since other workloads won't use this at all
# dd if=/dev/zero of=$dev_name bs=1048576 count=81910 status=progress

# warm the cache by reading in the entire image
# TODO this is a temporary workaround; figure out a better way to warm the cache
# for fio later
if [[ $read_entire_img -eq 1 ]]; then
	printf "\n\n===Reading entire image to warm cache===\n\n"
	# dd if=$dev_name of=/dev/null bs=1048576 status=progress
	dd if=$dev_name of=/dev/null bs=1048576 count=81910 status=progress
fi

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
		--size=$fio_size \
		--eta-newline=1 \
		--direct=1 \
		--group_reporting \
		--name=j1 \
		--filename=$dev_name \
		| tee /tmp/client-bench-results.txt

	printf "\nRESULT: Fio (iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt

	sleep 2
}

function run_fio_limit_iops {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4, iops_limit=$5===\n\n"
	fio \
		--name=global \
		--rw=$1 \
		--runtime=$2 \
		--iodepth=$3 \
		--bs=$4 \
		--rate_iops=$5 \
		--ioengine=io_uring \
		--time_based \
		--randseed=42 \
		--size=$fio_size \
		--eta-newline=1 \
		--direct=1 \
		--group_reporting \
		--name=j1 \
		--filename=$dev_name \
		| tee /tmp/client-bench-results.txt

	printf "\nRESULT: Fio (iodepth=$3; bs=$4, iops_limit=$5) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt

	sleep 2
}

# We run all the read workloads first, then the writes, to preserve the cache
# This is a hack; we need to figure out a better solution later

run_fio randread 60 128 4ki
run_fio read 60 128 4ki

# Limit iops and see latency

run_fio_limit_iops randread 30 128 4ki 10000
run_fio_limit_iops randread 30 128 4ki 20000
run_fio_limit_iops randread 30 128 4ki 30000
run_fio_limit_iops randread 30 128 4ki 40000
run_fio_limit_iops randread 30 128 4ki 50000
run_fio_limit_iops randread 30 128 4ki 60000
run_fio_limit_iops randread 30 128 4ki 70000
run_fio_limit_iops randread 30 128 4ki 80000
run_fio_limit_iops randread 30 128 4ki 90000
run_fio_limit_iops randread 30 128 4ki 100000

run_fio_limit_iops read 30 128 4ki 10000
run_fio_limit_iops read 30 128 4ki 20000
run_fio_limit_iops read 30 128 4ki 30000
run_fio_limit_iops read 30 128 4ki 40000
run_fio_limit_iops read 30 128 4ki 50000
run_fio_limit_iops read 30 128 4ki 60000
run_fio_limit_iops read 30 128 4ki 70000
run_fio_limit_iops read 30 128 4ki 80000
run_fio_limit_iops read 30 128 4ki 90000
run_fio_limit_iops read 30 128 4ki 100000

# Test different queue depths
run_fio randread 30 1 4ki
run_fio randread 30 16 4ki
run_fio randread 30 32 4ki
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

run_fio read 30 1 4ki
run_fio read 30 16 4ki
run_fio read 30 32 4ki
run_fio read 30 64 4ki
run_fio read 30 80 4ki
run_fio read 30 96 4ki
run_fio read 30 112 4ki
run_fio read 30 128 4ki
run_fio read 30 160 4ki
run_fio read 30 192 4ki
run_fio read 30 224 4ki
run_fio read 30 256 4ki
run_fio read 30 320 4ki
run_fio read 30 384 4ki
run_fio read 30 448 4ki
run_fio read 30 512 4ki

# Test different block sizes

run_fio randread 30 256 4ki
run_fio randread 30 256 8ki
run_fio randread 30 256 16ki
run_fio randread 30 256 32ki
run_fio randread 30 256 64ki

run_fio read 30 256 4ki
run_fio read 30 256 8ki
run_fio read 30 256 16ki
run_fio read 30 256 32ki
run_fio read 30 256 64ki

# === Writes ===

run_fio randwrite 60 128 4ki
run_fio write 60 128 4ki

run_fio_limit_iops randwrite 30 128 4ki 10000
run_fio_limit_iops randwrite 30 128 4ki 20000
run_fio_limit_iops randwrite 30 128 4ki 30000
run_fio_limit_iops randwrite 30 128 4ki 40000
run_fio_limit_iops randwrite 30 128 4ki 50000
run_fio_limit_iops randwrite 30 128 4ki 60000
run_fio_limit_iops randwrite 30 128 4ki 70000
run_fio_limit_iops randwrite 30 128 4ki 80000
run_fio_limit_iops randwrite 30 128 4ki 90000
run_fio_limit_iops randwrite 30 128 4ki 100000

run_fio_limit_iops write 30 128 4ki 10000
run_fio_limit_iops write 30 128 4ki 20000
run_fio_limit_iops write 30 128 4ki 30000
run_fio_limit_iops write 30 128 4ki 40000
run_fio_limit_iops write 30 128 4ki 50000
run_fio_limit_iops write 30 128 4ki 60000
run_fio_limit_iops write 30 128 4ki 70000
run_fio_limit_iops write 30 128 4ki 80000
run_fio_limit_iops write 30 128 4ki 90000
run_fio_limit_iops write 30 128 4ki 100000

# Test different queue depths

run_fio randwrite 30 1 4ki
run_fio randwrite 30 16 4ki
run_fio randwrite 30 32 4ki
run_fio randwrite 30 64 4ki
run_fio randwrite 30 80 4ki
run_fio randwrite 30 96 4ki
run_fio randwrite 30 112 4ki
run_fio randwrite 30 128 4ki
run_fio randwrite 30 160 4ki
run_fio randwrite 30 192 4ki
run_fio randwrite 30 224 4ki
run_fio randwrite 30 256 4ki
run_fio randwrite 30 320 4ki
run_fio randwrite 30 384 4ki
run_fio randwrite 30 448 4ki
run_fio randwrite 30 512 4ki

run_fio write 30 1 4ki
run_fio write 30 16 4ki
run_fio write 30 32 4ki
run_fio write 30 64 4ki
run_fio write 30 80 4ki
run_fio write 30 96 4ki
run_fio write 30 112 4ki
run_fio write 30 128 4ki
run_fio write 30 160 4ki
run_fio write 30 192 4ki
run_fio write 30 224 4ki
run_fio write 30 256 4ki
run_fio write 30 320 4ki
run_fio write 30 384 4ki
run_fio write 30 448 4ki
run_fio write 30 512 4ki


# Test different block sizes

run_fio randwrite 30 256 4ki
run_fio randwrite 30 256 8ki
run_fio randwrite 30 256 16ki
run_fio randwrite 30 256 32ki
run_fio randwrite 30 256 64ki

run_fio write 30 256 4ki
run_fio write 30 256 8ki
run_fio write 30 256 16ki
run_fio write 30 256 32ki
run_fio write 30 256 64ki

# filesystem benchmarks

# make sure old filesystem and mounts are gone
# wipefs -a $dev_name
dd if=/dev/zero of=$dev_name bs=1M count=100
#umount $dev_name || true
umount /mnt/fsbench || true

printf "\n==== Creating filesystem ====\n"

mkfs.ext4 -E nodiscard $dev_name
mkdir -p /mnt/fsbench
mount $dev_name /mnt/fsbench
rm -rf /mnt/fsbench/*

printf "\n\n"
printf "=========================================\n"
printf "=== Running filebench workloads       ===\n"
printf "=========================================\n"
printf "\n\n"

# filebench hangs if ASLR is on
# don't ask who spent a whole day trying to debug LSVD only to find out it was ASLR
# definitely not Isaac, he would never do that :(
echo 0 >/proc/sys/kernel/randomize_va_space

function run_filebench {
	printf "\n\n===Filebench: workload=$1===\n\n"
	rm -rf /mnt/fsbench/*
	filebench -f $1 | tee /tmp/client-bench-results.txt

	printf "\nRESULT: Filebench $1:"
	perl -lane 'print if /IO Summary/' /tmp/client-bench-results.txt
}

# shorten runtime
perl -pi -e 's/run 300/run 180/' /tmp/filebench/*.f
for workload in /tmp/filebench/*.f; do
	run_filebench $workload
done

# === disconnect and cleanup ===
# in the trap SIGTERM above
