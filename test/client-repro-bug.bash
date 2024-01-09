#!/usr/bin/env bash

set -xeuo pipefail

trap 'umount /mnt/fsbench || true; nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true; exit' SIGINT SIGTERM EXIT

modprobe nvme-fabrics

gw_ip=${gw_ip:-10.1.0.5}
nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true
nvme connect -t tcp --traddr $gw_ip -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 5

nvme list
# nvme connect sometimes gives the wrong name (you had one job >_<)
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $dev_name\n"

dd if=/dev/zero of=$dev_name bs=1M count=100
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

# make the workloads smaller for abbreviated run
# perl -pi -e 's/nfiles=200000/nfiles=50000/' /tmp/filebench/*.f
perl -pi -e 's/run 300/run 60/' /tmp/filebench/*.f

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
