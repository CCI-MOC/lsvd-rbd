#!/usr/bin/env bash
set -xeuo pipefail

# Record a full run under perf of fio with rbd backend LSVD preloaded
lsvd_dir=$(git rev-parse --show-toplevel)

cd $lsvd_dir
make clean
make -j$(nproc) nosan

./remove_objs.py pone perf-fio
./imgtool --rados --create --size=1G pone/perf-fio
# ./thick-image --size=1G pone/perf-fio

env LD_PRELOAD="$lsvd_dir/liblsvd.so" \
gdb --args \
	fio --name=fwl --rw=randread --size=10G --bs=4k --iodepth=64 --numjobs=1 \
		--randseed=42 --runtime=60 --direct=1 \
		--ioengine=rbd --pool=pone --rbdname=perf-fio || true

# LD_PRELOAD="/usr/lib/x86_64-linux-gnu/libasan.so.8 /usr/lib/x86_64-linux-gnu/libubsan.so.1 $lsvd_dir/liblsvd.so" \
# 	fio --name=fwl --rw=randread --size=10G --bs=4k --iodepth=64 --numjobs=1 \
# 		--randseed=42 --runtime=60 --direct=1 \
# 		--ioengine=rbd --pool=pone --rbdname=perf-fio || true

