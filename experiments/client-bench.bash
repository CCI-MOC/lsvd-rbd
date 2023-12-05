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

trap 'umount /mnt/fsbench || true; nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true; exit' SIGINT SIGTERM EXIT

# if [ "$EUID" -ne 0 ]
#   then echo "Please run as root"
#   exit
# fi

modprobe nvme-fabrics

gw_ip=${gw_ip:-10.1.0.5}
# see that it's there
nvme discover -t tcp -a $gw_ip -s 9922
nvme connect -t tcp --traddr $gw_ip -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 5

nvme list
# nvme connect sometimes gives the wrong name (you had one job >_<)
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $dev_name\n"

# === run the benchmarks ===

#num_fio_processes=4
num_fio_processes=1
fio_size="80GB"
fio_bs=${blksize:-4k}

# fio

function run_fio {
	printf "\n\n===Fio: workload=$1, time=$2, iodepth=$3, bs=$4===\n\n"
	fio \
		--name=fio-"$1" \
		--rw=$1 \
		--filename=$dev_name \
		--size=$fio_size \
		--direct=1 \
		--bs=$4 \
		--ioengine=io_uring \
		--iodepth=$3 \
		--runtime=$2 \
		--time_based \
		--numjobs=$num_fio_processes \
		--group_reporting \
		--randrepeat=0 \
		--eta-newline=1 | tee /tmp/client-bench-results.txt

	printf "\nRESULT: Fio (iodepth=$3) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt

	sleep 15
}

run_fio randread 60 256 $fio_bs
run_fio randwrite 60 256 $fio_bs
run_fio read 60 256 $fio_bs
run_fio write 60 256 $fio_bs

printf "\n\n"
printf "=========================================\n"
printf "=== Trying out different queue depths ===\n"
printf "=========================================\n"
printf "\n\n"

run_fio randwrite 60 1 $fio_bs
run_fio randwrite 60 32 $fio_bs
run_fio randwrite 60 64 $fio_bs
run_fio randwrite 60 128 $fio_bs

run_fio randread 60 1 $fio_bs
run_fio randread 60 32 $fio_bs
run_fio randread 60 64 $fio_bs
run_fio randread 60 128 $fio_bs

printf "\n\n"
printf "========================================\n"
printf "=== Trying out different block sizes ===\n"
printf "========================================\n"
printf "\n\n"

run_fio read 60 64 16k
run_fio read 60 64 64k
run_fio write 60 64 16k
run_fio write 60 64 64k

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

for workload in /tmp/filebench/*.f; do
	run_filebench $workload
done

# === disconnect and cleanup ===
# in the trap SIGTERM above
