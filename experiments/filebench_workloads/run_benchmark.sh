#!/bin/bash
#usage: ./run_benchmark.sh /dev/rbd0 varmail.f

for i in {1..3}
do
	echo "Round $i starts ....."
	echo ""
	sudo umount /mnt_benchmarks/
	(echo y ) | mkfs.ext4 $1
	sudo mount $1 /mnt_benchmarks/
	sleep 3
	echo 0 > /proc/sys/kernel/randomize_va_space
	filebench -f $2
	sync; echo 3 > /proc/sys/vm/drop_cachesZ
#	pkill dis
	sleep 5
done