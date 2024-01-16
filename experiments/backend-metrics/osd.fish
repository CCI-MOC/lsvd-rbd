#!/usr/bin/env fish

set fish_trace 1
set pids (ps aux | perl -lane 'print "$F[1]" if /ceph-osd -n osd.(\d+) -f/ and not /docker/ and $1 ~~ [13,15,79,80,5,7,81,82,28,29,30,31,6,12,14,20]' | tee /tmp/osd-pids.txt)
cat /proc/$pids/stat | perl -lane 'print $F[13]+$F[14]' | tee /tmp/osd-cpu-stats.txt
