#!/usr/bin/env bash

set -xeuo pipefail

nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true
nvme connect -t tcp --traddr 10.1.0.5 -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 5
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')

trap 'umount /mnt/fsbench || true; nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true; exit' SIGINT SIGTERM SIGHUP EXIT

dd if=$dev_name of=/dev/null bs=1048576 count=81910 status=progress
# dd if=$dev_name of=/dev/null bs=1048576 count=819 status=progress

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

