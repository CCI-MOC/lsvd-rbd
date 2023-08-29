#!/usr/bin/env bash

set -euo pipefail

for pair in $*; do
    if [ ${pair#*=} != $pair ] ; then
        eval $pair
    else
        echo ERROR: $pair not an assignment
    fi
done

printf "===Starting client benchmark\n\n"

trap 'umount /mnt/fsbench || nvme disconnect -n nqn.2016-06.io.spdk:cnode1; exit' SIGINT SIGTERM EXIT

# if [ "$EUID" -ne 0 ]
#   then echo "Please run as root"
#   exit
# fi

modprobe nvme-fabrics

# see that it's there
nvme discover -t tcp -a 10.1.0.5 -s 9922

#dev_name=$(nvme connect -t tcp  --traddr 10.1.0.5 -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal | perl -lane 'print @F[1]')"n1"
nvme connect -t tcp  --traddr 10.1.0.5 -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
# nvme connect sometimes gives the wrong name
dev_name=$(nvme list | grep SPDK | awk '{print $1}')
printf "Using device $dev_name\n"

# === run the benchmarks ===

num_fio_processes=4
fio_size="80GB"
fio_iodepth=${iodepth:-256}
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
		--eta-newline=1

	sleep 15
}

run_fio randread 60 $fio_iodepth $fio_bs
run_fio randwrite 60 $fio_iodepth $fio_bs
# run_fio read 60 $fio_iodepth $fio_bs
# run_fio write 60 $fio_iodepth $fio_bs

printf "\n\n"
printf "========================================="
printf "=== Trying out different queue depths ==="
printf "========================================="
printf "\n\n"

run_fio randread 60 1 $fio_bs
run_fio randwrite 60 32 $fio_bs
run_fio randwrite 60 64 $fio_bs
run_fio randwrite 60 128 $fio_bs

# printf "\n\n"
# printf "========================================"
# printf "=== Trying out different block sizes ==="
# printf "========================================"
# printf "\n\n"

# run_fio read 60 $fio_iodepth 8k
# run_fio write 60 $fio_iodepth 16k

# filesystem benchmarks

mkfs.ext4 $dev_name
mkdir -p /mnt/fsbench
mount $dev_name /mnt/fsbench

filebench -f /tmp/filebench-varmail.txt
filebench -f /tmp/filebench-fileserver.txt
filebench -f /tmp/filebench-oltp.txt

# === disconnect and cleanup ===
# in the trap SIGTERM above

