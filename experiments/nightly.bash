#!/usr/bin/env bash

./bench-rbd.bash triple-ssd
./bench-lsvd.bash triple-ssd
./bench-rbd.bash triple-hdd
./bench-lsvd.bash triple-hdd
./bench-nvme.bash /dev/nvme0n1


