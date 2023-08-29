#!/usr/bin/env bash

set -euo pipefail

if [ "$EUID" -ne 0 ]
  then echo "Please run as root"
  exit
fi

./bench-lsvd.bash rbd
./bench-lsvd.bash test_hdd_pool
./bench-rbd.bash rbd
./bench-rbd.bash test_hdd_pool
