#!/usr/bin/env bash

set -euo pipefail

printf "===Starting client benchmark\n\n"

trap 'umount /mnt/bench_filesystem; nvme disconnect -n nqn.2016-06.io.spdk:cnode1; exit' SIGINT SIGTERM EXIT

# if [ "$EUID" -ne 0 ]
#   then echo "Please run as root"
#   exit
# fi

modprobe nvme-fabrics

# see that it's there
# nvme discover -t tcp -a 10.1.0.5 -s 9922

dev_name=$(nvme connect -t tcp  --traddr 10.1.0.5 -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal | perl -lane 'print @F[1]')"n1"
printf "Using device $dev_name\n"

# === run the benchmarks ===

num_fio_processes=4
fio_size="1GB"
fio_iodepth=256

printf "\n\n\n===Random reads===\n\n"
fio \
	--name=fio-randread \
	--rw=randread \
	--filename=/dev/$dev_name \
	--size=$fio_size \
	--direct=1 \
	--bs=4k \
	--ioengine=io_uring \
	--iodepth=$fio_iodepth \
	--runtime=60 \
	--time_based \
	--numjobs=$fio_iodepth \
	--group_reporting \
	--eta-newline=1 \
	--readonly

sleep 15

printf "\n\n\n===Random writes===\n\n"
fio \
	--name=fio-randwrite \
	--rw=randwrite \
	--filename=/dev/$dev_name \
	--size=$fio_size \
	--direct=1 \
	--bs=4k \
	--ioengine=io_uring \
	--iodepth=$fio_iodepth \
	--runtime=60 \
	--time_based \
	--numjobs=$fio_iodepth \
	--group_reporting \
	--eta-newline=1 \

sleep 15

printf "\n\n\n===Sequential reads===\n\n"
fio \
	--name=fio-seqread \
	--rw=read \
	--filename=/dev/$dev_name \
	--size=$fio_size \
	--direct=1 \
	--bs=4k \
	--ioengine=io_uring \
	--iodepth=$fio_iodepth \
	--runtime=60 \
	--time_based \
	--numjobs=$fio_iodepth \
	--group_reporting \
	--eta-newline=1 \
	--readonly

sleep 15

printf "\n\n\n===Sequential writes===\n\n"
fio \
	--name=fio-seqwrite \
	--rw=write \
	--filename=/dev/$dev_name \
	--size=$fio_size \
	--direct=1 \
	--bs=4k \
	--ioengine=io_uring \
	--iodepth=$fio_iodepth \
	--runtime=60 \
	--time_based \
	--numjobs=$fio_iodepth \
	--group_reporting \
	--eta-newline=1 \


# filesystem benchmarks

mkfs.ext4 /dev/$dev_name
mkdir -p /mnt/fsbench
mount /dev/$dev_name /mnt/fsbench

/usr/local/bin/filebench -f /tmp/filebench-varmail.txt
/usr/local/bin/filebench -f /tmp/filebench-fileserver.txt
/usr/local/bin/filebench -f /tmp/filebench-oltp.txt

# === disconnect and cleanup ===
# in the trap SIGTERM above

