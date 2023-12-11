#!/usr/bin/env bash

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root"
  exit
fi

# designed to run nightly; we don't care if one of them fails
# set -euo pipefail

all_out=./experiment-results.txt
printf "\n\n\nBenchmark script raw output will be written to $all_out" | tee -a $all_out

export lsvd_cache_size=$((240 * 1024 * 1024 * 1024))
./bench-lsvd.bash rssd2 |& tee -a $all_out
export lsvd_cache_size=$((240 * 1024 * 1024 * 1024))
./bench-lsvd.bash triple-hdd |& tee -a $all_out

export lsvd_cache_size=$((20 * 1024 * 1024 * 1024))
./bench-lsvd.bash rssd2 |& tee -a $all_out
export lsvd_cache_size=$((20 * 1024 * 1024 * 1024))
./bench-lsvd.bash triple-hdd |& tee -a $all_out

./bench-rbd.bash rssd2 |& tee -a $all_out
./bench-rbd.bash triple-hdd |& tee -a $all_out
./bench-ramdisk.bash |& tee -a $all_out
./bench-nvme.bash /dev/nvme0n1 |& tee -a $all_out
