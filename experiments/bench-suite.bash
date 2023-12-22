#!/usr/bin/env bash

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root"
  exit
fi

lsvd_dir=$(git rev-parse --show-toplevel)
cd $lsvd_dir/experiments/

# designed to run nightly; we don't care if one of them fails
# set -euo pipefail
set -x

all_out=./experiment-results.txt
# printf "\n\n\nBenchmark script raw output will be written to $all_out\n\n\n" | tee -a $all_out

truncate -s 0 $all_out

# enable coredumps for debugging
ulimit -c unlimited

printf "\n\n\n===New Experiment===\n\n\n"
export lsvd_cache_size=$((240 * 1024 * 1024 * 1024))
./bench-lsvd.bash rssd2 |& tee -a $all_out
printf "\n\n\n===New Experiment===\n\n\n"
export lsvd_cache_size=$((240 * 1024 * 1024 * 1024))
./bench-lsvd.bash triple-hdd |& tee -a $all_out

printf "\n\n\n===New Experiment===\n\n\n"
export lsvd_cache_size=$((20 * 1024 * 1024 * 1024))
./bench-lsvd.bash rssd2 |& tee -a $all_out
printf "\n\n\n===New Experiment===\n\n\n"
export lsvd_cache_size=$((20 * 1024 * 1024 * 1024))
./bench-lsvd.bash triple-hdd |& tee -a $all_out

# don't collect these over and over again
exit

printf "\n\n\n===New Experiment===\n\n\n"
./bench-rbd.bash rssd2 |& tee -a $all_out
printf "\n\n\n===New Experiment===\n\n\n"
./bench-rbd.bash triple-hdd |& tee -a $all_out

printf "\n\n\n===New Experiment===\n\n\n"
./bench-ramdisk.bash |& tee -a $all_out
printf "\n\n\n===New Experiment===\n\n\n"
./bench-nvme.bash /dev/nvme0n1 |& tee -a $all_out
