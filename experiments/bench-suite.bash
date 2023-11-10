#!/usr/bin/env bash

set -euo pipefail

if [ "$EUID" -ne 0 ]
  then echo "Please run as root"
  exit
fi

./bench-lsvd.bash triple-ssd
./bench-rbd.bash triple-ssd
