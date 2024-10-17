#!/bin/bash
set -xeuo pipefail

workload=$1
echo "Benchmarking automount with fio $workload"

nvme connect -t tcp  --traddr localhost -s 4420 -n nqn.2019-05.io.lsvd:cnode1 -o normal
trap 'nvme disconnect -n nqn.2019-05.io.lsvd:cnode1' SIGINT SIGTERM EXIT

sleep 1
nvme list

IOPATH=/dev/nvme1n1 fio $workload
