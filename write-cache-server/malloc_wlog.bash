#!/usr/bin/env bash

set -xeuo pipefail

lsvd_dir=$(git rev-parse --show-toplevel)
cd $lsvd_dir/spdk

./build/bin/nvmf_tgt -m '[0,1,2,3]' &
sleep 5

scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode8 -a -s SPDK00000000000008 -d SPDK_Controller8-wlog
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode8 -t tcp -a 10.1.0.8 -s 23789

scripts/rpc.py bdev_malloc_create -b bdev_malloc 2048 4096
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode8 bdev_malloc
