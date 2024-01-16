#!/usr/bin/env bash

set -xeuo pipefail

modprobe nvme-fabrics
nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true
nvme disconnect -n nqn.2016-06.io.spdk:cnode2 || true
nvme disconnect -n nqn.2016-06.io.spdk:cnode3 || true

nvme connect -t tcp --traddr $gw_ip -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 5
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $dev_name\n"

fio --name=global \
	--rw=randread --iodepth=128 --bs=4ki \
	--ioengine=io_uring --randseed=42 \
	--size=5G --eta-newline=1 --direct=1 --group_reporting \
	--name=j1 --filename=/dev/nvme1n1

fio --name=global \
	--rw=randwrite --runtime=60 --iodepth=128 --bs=4ki \
	--ioengine=io_uring --time_based --randseed=42 \
	--size=5G --eta-newline=1 --direct=1 --group_reporting \
	--name=j1 --filename=/dev/nvme1n1


nvme disconnect -n nqn.2016-06.io.spdk:cnode1
