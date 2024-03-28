# Benchmarking

The benchmarking setup is largely automated. All scripts are in `experiments/`.

The numbers in the atc24 paper are generated with the master script
`bench-suite.bash`. It calls all the subscripts `bench-*.bash` with different
parameters that we care about.

The `bench-*.bash` scripts themselves largely call various functions from
`common.bash`, which contain the vast majority of the actual running logic.
The setup NVMe-oF, SPDK, and LSVD, then `ssh` into another remote machine to
run `client-bench.bash`, which actually runs the workloads we're measuring.

Sumatra has most of the scripts that actually generate the graphs.
