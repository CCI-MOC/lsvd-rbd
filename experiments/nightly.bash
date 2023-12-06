#!/usr/bin/env bash

for ((i=1; i<=10; i++)); do
    echo "Running script for the $i time"
    ./bench-rbd.bash triple-ssd
    ./bench-lsvd.bash triple-ssd
    ./bench-rbd.bash triple-hdd
    ./bench-lsvd.bash triple-hdd
    ./bench-nvme.bash /dev/nvme0n1
    
    sleep 60
done
