#!/usr/bin/env fish

if not set -q argv[1]
	echo "Usage: collect.fish <prefix>"
	exit 1
end

# set fish_trace 1
set prefix $argv[1]

# rssd2 pool OSD ids
# dl380p-1: 13 15 79 80
# dl380p-2: 5 7 81 82
# dl380p-3: 28 29 30 31
# dl380p-4: 6 12 14 20

# run on osds

set lsvd_dir (git rev-parse --show-toplevel)
cd $lsvd_dir

set osd_hosts dl380p-1 dl380p-2 dl380p-3 dl380p-4

for host in $osd_hosts
	# scp experiments/backend-metrics/osd.fish $host:/tmp/osd.fish
	ssh $host "fish /tmp/osd.fish"
end

mkdir -p ./experiments/results-cpu
rm -rf ./experiments/results-cpu/*
for host in $osd_hosts
	scp $host:/tmp/osd-cpu-stats.txt ./experiments/results-cpu/$prefix-$host-osd-cpu-stats.txt
end

# sum them up
# cat ./experiments/results-cpu/$prefix-*
cat ./experiments/results-cpu/$prefix-* | perl -lane '$sum += $_; END { print $sum }'
