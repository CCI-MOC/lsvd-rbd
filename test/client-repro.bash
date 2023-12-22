#!/usr/bin/env bash

set -xeuo pipefail

nvme disconnect -n nqn.2016-06.io.spdk:cnode1 || true
nvme connect -t tcp --traddr 10.1.0.5 -s 9922 -n nqn.2016-06.io.spdk:cnode1 -o normal
sleep 5
dev_name=$(nvme list | perl -lane 'print @F[0] if /SPDK/')

dd if=$dev_name of=/dev/null bs=1048576 count=81910 status=progress
