#!/usr/bin/env bash

set -euo pipefail

if [ "$EUID" -ne 0 ]
  then echo "Please run as root"
  exit
fi

# designed to run nightly; we don't care if one of them fails
set +e

all_out=./experiment-results.txt

./bench-lsvd.bash rssd2 |& tee -a $all_out
./bench-lsvd.bash triple-hdd |& tee -a $all_out
./bench-rbd.bash rssd2 |& tee -a $all_out
./bench-rbd.bash triple-hdd |& tee -a $all_out
./bench-ramdisk.bash |& tee -a $all_out
./bench-nvme.bash /dev/nvme0n1 |& tee -a $all_out
