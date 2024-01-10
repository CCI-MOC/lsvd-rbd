#!/usr/bin/env bash

set -xeuo pipefail
ulimit -c

cur_time=$(date +"%FT%T")
lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10.1.[0-9.]+)\/24/' | head -n 1)
client_ip=${client_ip:-10.1.0.6}
outfile=$lsvd_dir/experiments/results/$cur_time.nvme-multi.txt

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=lsvd-benchmark
imgsize=20G
blocksize=4096

source $lsvd_dir/experiments/common.bash

fstrim /mnt/nvme

dd if=/dev/random of=/mnt/nvme/$imgname.multi.1 bs=1M count=20480 status=progress &
dd if=/dev/random of=/mnt/nvme/$imgname.multi.2 bs=1M count=20480 status=progress &
dd if=/dev/random of=/mnt/nvme/$imgname.multi.3 bs=1M count=20480 status=progress &
dd if=/dev/random of=/mnt/nvme/$imgname.multi.4 bs=1M count=20480 status=progress &

wait

kill_nvmf

launch_gw_background
configure_nvmf_common $gw_ip

scripts/rpc.py bdev_aio_create /mnt/nvme/$imgname.multi.1 f1 4096
scripts/rpc.py bdev_aio_create /mnt/nvme/$imgname.multi.2 f2 4096
scripts/rpc.py bdev_aio_create /mnt/nvme/$imgname.multi.3 f3 4096
scripts/rpc.py bdev_aio_create /mnt/nvme/$imgname.multi.4 f4 4096

scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 f1
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 f2
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 f3
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 f4

trap "cleanup_nvmf; exit" SIGINT SIGTERM EXIT
run_client_bench $client_ip $outfile client-bench-multi.bash "read_entire_img=0"
