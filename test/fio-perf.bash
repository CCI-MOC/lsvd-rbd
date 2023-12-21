#!/usr/bin/env bash
set -xeuo pipefail

# Record a full run under perf of fio with rbd backend LSVD preloaded
lsvd_dir=$(git rev-parse --show-toplevel)

cd $lsvd_dir
make clean
make -j$(nproc) release

./remove_objs.py pone perf-fio
# ./imgtool --rados --create --size=1G pone/perf-fio
./thick-image --size=1G pone/perf-fio

cd test/

rm perf.data || true
rm /tmp/*.wcache || true

# LD_PRELOAD=$lsvd_dir/liblsvd.so \
# perf record -g --call-graph dwarf -F 999 -o ./perf.data -- \
# 	fio --name=fwl --rw=randwrite --size=10G --bs=4k --iodepth=64 --numjobs=1 \
# 		--randseed=42 --runtime=60 --direct=1 \
# 		--ioengine=rbd --pool=pone --rbdname=perf-fio || true

LD_PRELOAD=$lsvd_dir/liblsvd.so \
perf record -g --call-graph dwarf -F 999 -o ./perf.data -- \
	fio --name=fwl --rw=randread --size=10G --bs=4k --iodepth=64 --numjobs=1 \
		--randseed=42 --runtime=60 --direct=1 \
		--ioengine=rbd --pool=pone --rbdname=perf-fio || true

perf script -F +pid --no-inline --input=./perf.data > ./perf.script
