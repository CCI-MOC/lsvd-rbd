#!/usr/bin/env bash

set -xeuo pipefail

make clean
make -j20 nosan

# create qemu image

lsvd_dir=$(git rev-parse --show-toplevel)
cd $lsvd_dir

LD_PRELOAD=$lsvd_dir/builddir/liblsvd.so \
    qemu-img create -f raw rbd:triple-ssd/lsvd-qemu-debug 20G

mkdir -p /mnt/nvme/lsvd-rcache /mnt/nvme/lsvd-wcache
export LSVD_RCACHE_DIR=/mnt/nvme/lsvd-rcache
export LSVD_WCACHE_DIR=/mnt/nvme/lsvd-wcache
export LSVD_GC_THRESHOLD=40

LD_PRELOAD=$lsvd_dir/builddir/liblsvd.so \
    qemu-system-x86_64 \
    -enable-kvm \
    -m 1024 \
    -cdrom qemu/ubuntu2204.iso \
    -vnc :1 -serial mon:stdio \
    -drive format=raw,file=seed.iso,cache=none,if=virtio \
    -drive format=raw,file=rbd:triple-ssd/lsvd-qemu-debug

exit

# This is the non-lsvd version

# This is the rbd version
qemu-img create -f raw rbd:triple-ssd/rbd-ubuntu-img 5G
qemu-system-x86_64 \
    -enable-kvm \
    -m 1024 \
    -cdrom qemu/ubuntu2204.iso \
    -vnc :1 -serial mon:stdio \
    -drive format=raw,file=seed.iso,cache=none,if=virtio \
    -drive format=raw,file=rbd:triple-ssd/rbd-ubuntu-img

# qemu-img create -f qcow2 local.qcow2 20G
qemu-system-x86_64 \
    -enable-kvm \
    -m 1024 \
    -cdrom qemu/ubuntu2204.iso \
    -vnc :1 -serial mon:stdio \
    -drive format=raw,file=seed.iso,cache=none,if=virtio \
    -drive format=qcow2,file=local.qcow2
