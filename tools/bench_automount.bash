#!/bin/bash
set -xeuo pipefail

nvme connect -t tcp  --traddr localhost -s 4420 -n nqn.2019-05.io.lsvd:cnode1 -o normal
sleep 1
nvme list

IOPATH=/dev/nvme1n1 fio /home/isaackhor/code/lsvd-research/fio/wr_read.fio
# IOPATH=/dev/nvme1n1 fio /home/isaackhor/code/lsvd-research/fio/write_only.fio
# IOPATH=/dev/nvme1n1 fio /home/isaackhor/code/lsvd-research/fio/read_only.fio

nvme disconnect -n nqn.2019-05.io.lsvd:cnode1
