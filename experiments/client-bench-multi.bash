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
		dd if=$dev_name of=/dev/null bs=1048576 count=20475 status=progress &
	done
	wait
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
		--size=20g \
		--eta-newline=1 \
		--direct=1 \
		--group_reporting \
		--name=j1 \
		--filename=/dev/nvme1n1 \
		--name=j2 \
		--filename=/dev/nvme1n2 \
		--name=j3 \
		--filename=/dev/nvme1n3 \
		--name=j4 \
		--filename=/dev/nvme1n4 \
		| tee /tmp/client-bench-results.txt

	printf "\nRESULT: Fio (iodepth=$3; bs=$4) $1:"
	perl -lane 'print if /IOPS/' /tmp/client-bench-results.txt

	sleep 2
}

# We run all the read workloads first, then the writes, to preserve the cache
# This is a hack; we need to figure out a better solution later

run_fio randread 60 256 4k
run_fio read 60 256 4k

exit

run_fio randread 60 1 4k
# run_fio randread 60 32 4k
run_fio randread 60 64 4k
# run_fio randread 60 128 4k

run_fio read 60 1 4k
# run_fio read 60 32 4k
run_fio read 60 64 4k
# run_fio read 60 128 4k

run_fio read 60 256 4k
run_fio read 60 256 16k
run_fio read 60 256 64k

run_fio randread 60 256 4k
run_fio randread 60 256 16k
run_fio randread 60 256 64k

run_fio randwrite 60 1 4k
# run_fio randwrite 60 32 4k
run_fio randwrite 60 64 4k
# run_fio randwrite 60 128 4k

run_fio write 60 1 4k
# run_fio write 60 32 4k
run_fio write 60 64 4k
# run_fio write 60 128 4k

run_fio write 60 256 16k
run_fio write 60 256 64k

run_fio randwrite 60 256 16k
run_fio randwrite 60 256 64k

run_fio randwrite 60 256 4k
run_fio write 60 256 4k

# === disconnect and cleanup ===
# in the trap SIGTERM above
