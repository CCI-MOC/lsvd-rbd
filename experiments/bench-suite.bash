#!/usr/bin/env bash

set -x

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root"
  exit
fi

lsvd_dir=$(git rev-parse --show-toplevel)
cd $lsvd_dir/experiments/

ctime=$(date +"%FT%T")
all_out=./results/$ctime-agg-results.txt
# printf "\n\n\nBenchmark script raw output will be written to $all_out\n\n\n" | tee -a $all_out

# enable coredumps for debugging
ulimit -c unlimited

printf "\n\n\n=== LSVD: 240g cache, malloc write log ===\n\n\n"
export lsvd_cache_size=$((240 * 1024 * 1024 * 1024))
export lsvd_wlog_root=/mnt/nvme-malloc/lsvd-write/
./bench-lsvd.bash rssd2 malloc |& tee -a $all_out
./bench-lsvd.bash triple-hdd malloc |& tee -a $all_out

printf "\n\n\n=== LSVD: 240g cache, nvme write log ===\n\n\n"
export lsvd_cache_size=$((240 * 1024 * 1024 * 1024))
export lsvd_wlog_root=/mnt/nvme-remote/lsvd-write/
./bench-lsvd.bash rssd2 nvme |& tee -a $all_out
./bench-lsvd.bash triple-hdd nvme |& tee -a $all_out

printf "\n\n\n=== LSVD: 20g cache, malloc write log ===\n\n\n"
export lsvd_cache_size=$((20 * 1024 * 1024 * 1024))
export lsvd_wlog_root=/mnt/nvme-malloc/lsvd-write/
./bench-lsvd.bash rssd2 malloc |& tee -a $all_out
./bench-lsvd.bash triple-hdd malloc |& tee -a $all_out

printf "\n\n\n=== LSVD: 20g cache, nvme write log ===\n\n\n"
export lsvd_cache_size=$((20 * 1024 * 1024 * 1024))
export lsvd_wlog_root=/mnt/nvme-remote/lsvd-write/
./bench-lsvd.bash rssd2 nvme |& tee -a $all_out
./bench-lsvd.bash triple-hdd nvme |& tee -a $all_out

printf "\n\n\n=== LSVD multi-client: 20g cache, malloc write log ===\n\n\n"
export lsvd_cache_size=$((120 * 1024 * 1024 * 1024))
export lsvd_wlog_root=/mnt/nvme-malloc/lsvd-write/
./multi-client/bench-lsvd-multi.bash rssd2 |& tee -a $all_out
./multi-client/bench-lsvd-multi.bash triple-hdd |& tee -a $all_out

printf "\n\n\n=== RBD multi-client ===\n\n\n"
./multi-client/bench-rbd-multi.bash rssd2 |& tee -a $all_out
./multi-client/bench-rbd-multi.bash triple-hdd |& tee -a $all_out

exit

printf "\n\n\n===New Experiment===\n\n\n"
./bench-rbd.bash rssd2 |& tee -a $all_out
printf "\n\n\n===New Experiment===\n\n\n"
./bench-rbd.bash triple-hdd |& tee -a $all_out

printf "\n\n\n===New Experiment===\n\n\n"
./bench-nvme-multi.bash |& tee -a $all_out

printf "\n\n\n===New Experiment===\n\n\n"
./bench-ramdisk.bash |& tee -a $all_out
printf "\n\n\n===New Experiment===\n\n\n"
./bench-nvme.bash /dev/nvme0n1 |& tee -a $all_out