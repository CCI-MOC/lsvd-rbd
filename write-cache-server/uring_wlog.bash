#!/usr/bin/env bash

set -xeuo pipefail

lsvd_dir=$(git rev-parse --show-toplevel)
cd $lsvd_dir/spdk

./build/bin/nvmf_tgt -m '[0,1,2,3]' &

scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode6 -a -s SPDK00000000000006 -d SPDK_Controller1-wlog
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode6 -t tcp -a 10.1.0.6 -s 23789

scripts/rpc.py bdev_aio_create /mnt/nvme/uring_wlog log1 4096
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode6 log1
