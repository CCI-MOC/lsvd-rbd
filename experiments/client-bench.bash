#!/usr/bin/env bash

set -euo pipefail

sudo modprobe nvme-fabrics

# see that it's there
# sudo nvme discover -t tcp -a 10.1.0.5 -s 9922

sudo nvme connect -t tcp  --traddr 10.1.0.5 -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal

# === run the benchmarks ===
# assumes that we got the device nvme0

printf "\n\n\n===Random reads===\n\n"
sudo fio \
	--name=fio-randread \
	--rw=randread \
	--filename=/dev/nvme0n1 \
	--direct=1 \
	--bs=4k \
	--ioengine=io_uring \
	--iodepth=256 \
	--runtime=60 \
	--time_based \
	--numjobs=4 \
	--group_reporting \
	--eta-newline=1 \
	--readonly

printf "\n\n\n===Random writes===\n\n"
sudo fio \
	--name=fio-randwrite \
	--rw=randwrite \
	--filename=/dev/nvme0n1 \
	--direct=1 \
	--bs=4k \
	--ioengine=io_uring \
	--iodepth=256 \
	--runtime=60 \
	--time_based \
	--numjobs=4 \
	--group_reporting \
	--eta-newline=1 \

printf "\n\n\n===Sequential reads===\n\n"
sudo fio \
	--name=fio-seqread \
	--rw=read \
	--filename=/dev/nvme0n1 \
	--direct=1 \
	--bs=4k \
	--ioengine=io_uring \
	--iodepth=256 \
	--runtime=60 \
	--time_based \
	--numjobs=4 \
	--group_reporting \
	--eta-newline=1 \
	--readonly

printf "\n\n\n===Sequential writes===\n\n"
sudo fio \
	--name=fio-seqwrite \
	--rw=write \
	--filename=/dev/nvme0n1 \
	--direct=1 \
	--bs=4k \
	--ioengine=io_uring \
	--iodepth=256 \
	--runtime=60 \
	--time_based \
	--numjobs=4 \
	--group_reporting \
	--eta-newline=1 \


# === disconnect and cleanup ===

sudo nvme disconnect -n nqn.2016-06.io.spdk:cnode1
