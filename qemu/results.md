# Results for cloned VMs

A few notes:

* I don't really have an automated way of doing this yes, manually looking at
  timestamps in the kernel log for now
* Clones are done via `clone.py` script
* GC disabled on clones, to be implemented
* Measured as time from QEMU start to the login prompt appearing

Results:

* Cold: 7163 requests, 6.8 to 7.1 seconds, 39% hitrate, 1.4 read amp
* Warm: same requests, 6.2 to 6.4 seconds, 97% hitrate
* Both read about 190 MiB of data on startup
* The bottleneck is not disk performance, but mostly CPU
* Massive savings on cache space though

