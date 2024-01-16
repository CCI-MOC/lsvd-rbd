#!/usr/bin/env bash

# starting cpu time

# rssd2 pool OSD ids
# dl380p-1: 13 15 79 80
# dl380p-2: 5 7 81 82
# dl380p-3: 28 29 30 31
# dl380p-4: 6 12 14 20

# pid -> osd number
pids=$(ps aux | perl -lane 'print "$F[1]" if /ceph-osd -n osd.(\d+) -f/ and not /docker/')

# run on machine 1
pids=$(ps aux | perl -lane 'print "$F[1], $_" if /ceph-osd -n osd.(\d+) -f/ and not /docker/ and $1 ~~ [13,15,79,80]')

pids=$(ps aux | perl -lane 'print "$F[1]" if /ceph-osd -n osd.(\d+) -f/ and not /docker/ and $1 ~~ [13,15,79,80]')
cat /proc/$pids/stat | perl -lane 'print $F[13]+$F[14]' | tee /tmp/cpu-usage.txt; done


