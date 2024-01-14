#!/usr/bin/env bash

set -xeuo pipefail

lsvd_dir=$(git rev-parse --show-toplevel)
source $lsvd_dir/experiments/common.bash

cur_time=$(date +"%FT%T")
outfile=$lsvd_dir/experiments/results/$cur_time.lsvd-multigw.rssd2.txt

ssh 10.1.0.5 "cd $lsvd_dir/experiments/multigw/; ./gateway-1.bash rssd2" &
ssh 10.1.0.6 "cd $lsvd_dir/experiments/multigw/; ./gateway-2.bash rssd2" &

ssh 10.1.0.8 "bash -s" < $lsvd_dir/experiments/multigw/coord.bash |& tee $outfile
