#!/usr/bin/env bash

./bench-rbd.bash triple-ssd
./bench-lsvd.bash triple-ssd
./bench-ramdisk.bash

# something about parse-results.bash
