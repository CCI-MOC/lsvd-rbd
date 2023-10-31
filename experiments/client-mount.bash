#!/usr/bin/env bash

set -xeuo pipefail

modprobe nvme-fabrics
nvme disconnect -n nqn.2016-06.io.spdk:cnode1
gw_ip=${gw_ip:-10.1.0.5}
nvme connect -t tcp  --traddr $gw_ip -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 2
nvme list
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')
printf "Using device $dev_name\n"