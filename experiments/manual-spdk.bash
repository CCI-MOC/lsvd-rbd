# run me as root

set -euo pipefail

# setup the bdevs. the image must already exist
scripts/rpc.py bdev_rbd_register_cluster rbd_cluster
# syntax: bdev_rbd_create <name> <pool> <image-name> <block_size> [-c <cluster_name>]
# image must already exist
scripts/rpc.py bdev_rbd_create rbd rbd-20g 512 -c rbd_cluster

# create subsystem
scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Ceph0

# setup listener
scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 10.1.0.8 -s 5001
