# performance tests

## to do:

- write batching, to reduce system CPU usage for small writes
- garbage collection
- fix hit/miss rate and read cache
- fix read pipeline problem

hit/miss rate: measure (a) cache hits, (b) cache line reads, (c) direct backend reads. Waste is (b) - (a),
keep that to no more than 50% (?) of (a)+(b)+(c)

## 4/16 (after libaio refactoring) performance issues:

###  read pipe problems:
setting `use_cache` to false to avoid the broken hitrate stuff, direct I/O to the backend is a lot slower
than native RBD. Using iodepth=64, we get the following speeds:

| lsvd | |
| --- | --- |
|6m40.901s 3m58.077s | = 400.901, 238.077 |
| 1570MiB/30009msec | = 401920 |
| 7m2.459s 4m10.652s | = 422.459, 250.652 |
|  |  = 401920 in 21.56, 12.57|
|  | = 11776 io/cpusec |
 
 | real | |
 | --- | --- |
|7m2.459s 4m10.652s | = 422.459, 250.652|
|4163MiB/30002msec)| = 1065728|
|8m24.278s 4m53.523s | = 504.278, 293.524|
| | = 1065728 in 81.82, 42.87|
| | = 8547 io/cpusec|

so it looks like it's just an issue with filling the pipe - we're actually using less CPU time.
Are there locking issues keeping us from filling the pipe with one thread?

## write performance

Real RBD looks pretty crappy on this cluster - with QD=64:
```
  write: IOPS=4567, BW=17.8MiB/s (18.7MB/s)(536MiB/30012msec); 0 zone resets
```

##  below tests were done with fio and write_perf.cc

### local write performance (dl380p-5, /dev/nvme2 Samsung 500G

buffered random write to NVME, no sync, queue depth 1. Second one is with indirection through ops structure, makes no difference 
```
  write: IOPS=210k, BW=822MiB/s (862MB/s)(24.1GiB/30000msec); 0 zone resets
  write: IOPS=239k, BW=932MiB/s (977MB/s)(27.3GiB/30000msec); 0 zone resets
```

direct random write to NVME, no sync. Queue depth 1, 2 - note no difference, as this version just uses pwrite and doesn't do async
```
1:   write: IOPS=20.2k, BW=79.0MiB/s (82.9MB/s)(2371MiB/30001msec); 0 zone resets
2:  write: IOPS=19.8k, BW=77.5MiB/s (81.3MB/s)(2326MiB/30001msec); 0 zone resets
```

POSIX aio with callbacks is horribly slow, which I kind of knew would be the case. With buffered and large queue depths it's sort of OK, but still bad; needs a much bigger queue depth for direct
```
[q=1,buffered]  write: IOPS=17.6k, BW=68.6MiB/s (71.9MB/s)(2057MiB/30001msec); 0 zone resets
[q=1,direct]  write: IOPS=10.0k, BW=39.1MiB/s (41.0MB/s)(1173MiB/30001msec); 0 zone resets
[q=16,buffered]  write: IOPS=14.0k, BW=54.5MiB/s (57.2MB/s)(1635MiB/30002msec); 0 zone resets
[q=16,direct]  write: IOPS=14.0k, BW=54.5MiB/s (57.2MB/s)(1635MiB/30002msec); 0 zone resets
[q=64,buffered]  write: IOPS=33.4k, BW=130MiB/s (137MB/s)(3910MiB/30002msec); 0 zone resets
[q=64,direct]  write: IOPS=12.2k, BW=47.6MiB/s (49.9MB/s)(1428MiB/30004msec); 0 zone resets
```

running my own completion thread:
```
[q=1,buffered]  write: IOPS=17.7k, BW=69.0MiB/s (72.3MB/s)(2069MiB/30001msec); 0 zone resets
[direct]  write: IOPS=8832, BW=34.5MiB/s (36.2MB/s)(1035MiB/30001msec); 0 zone resets
[q=16,direct]  write: IOPS=14.5k, BW=56.5MiB/s (59.2MB/s)(1694MiB/30001msec); 0 zone resets
[q=16,buffered]   write: IOPS=32.2k, BW=126MiB/s (132MB/s)(3775MiB/30001msec); 0 zone resets
```
going to 64 really didn't help things any

### single completion thread waiting in aio_suspend

[q=1,buffered]  write: IOPS=38.4k, BW=150MiB/s (157MB/s)(4502MiB/30000msec); 0 zone resets
[q=8,buffered] write: IOPS=146k, BW=572MiB/s (600MB/s)(16.8GiB/30001msec); 0 zone resets
[q=1,direct]  write: IOPS=13.4k, BW=52.2MiB/s (54.8MB/s)(1567MiB/30001msec); 0 zone resets
[q=8,direct]  write: IOPS=17.1k, BW=66.7MiB/s (70.0MB/s)(2002MiB/30001msec); 0 zone resets
[q=32,direct]  write: IOPS=18.9k, BW=74.0MiB/s (77.6MB/s)(2220MiB/30001msec); 0 zone resets

### fio direct access to device
note that fio runs at crazy speeds for buffered, then stalls; IOPS are for the full period including sync
```
[q=1,direct]  write: IOPS=25.1k, BW=98.0MiB/s (103MB/s)(2941MiB/30001msec); 0 zone resets
[q=1,direct]  write: IOPS=44.8k, BW=175MiB/s (183MB/s)(5248MiB/30001msec); 0 zone resets
[q=32,direct]  write: IOPS=44.9k, BW=175MiB/s (184MB/s)(5260MiB/30001msec); 0 zone resets
[q=1,buffered]  write: IOPS=35.0k, BW=137MiB/s (143MB/s)(8192MiB/59887msec); 0 zone resets
[sync,buffered]  write: IOPS=34.6k, BW=135MiB/s (142MB/s)(8192MiB/60699msec); 0 zone resets
[sync,direct]  write: IOPS=34.4k, BW=135MiB/s (141MB/s)(8192MiB/60893msec); 0 zone resets
```

### the intel ssd does a lot better:
```
[sync,buffered]  write: IOPS=135k, BW=526MiB/s (552MB/s)(15.4GiB/30001msec); 0 zone resets
[sync,direct]  write: IOPS=37.2k, BW=145MiB/s (152MB/s)(4356MiB/30001msec); 0 zone resets
[q=8,direct]  write: IOPS=123k, BW=481MiB/s (505MB/s)(14.1GiB/30001msec); 0 zone resets
[q=16,direct]  write: IOPS=118k, BW=462MiB/s (485MB/s)(13.5GiB/30001msec); 0 zone resets
```

back to fake RBD with completion thread:
```
[q=16,buffered]  write: IOPS=173k, BW=675MiB/s (708MB/s)(19.8GiB/30001msec); 0 zone resets
[q=8,buffered]  write: IOPS=119k, BW=465MiB/s (488MB/s)(13.6GiB/30001msec); 0 zone resets
[q=16,direct]  write: IOPS=27.7k, BW=108MiB/s (113MB/s)(3241MiB/30001msec); 0 zone resets
[q=32,direct]  write: IOPS=32.4k, BW=127MiB/s (133MB/s)(3799MiB/30002msec); 0 zone resets
[q=64,direct]  write: IOPS=36.2k, BW=141MiB/s (148MB/s)(4244MiB/30002msec); 0 zone resets
```

can't get over 100% CPU with direct, getting 200% with buffered.

Well, we can if we increase the number of threads writing:
```
[q=2*jobs=4] Jobs: 4 (f=4): [w(4)][100.0%][w=257MiB/s][w=65.8k IOPS][eta 00m:00s]


with libaio:
```
[q=1,direct]  write: IOPS=34.6k, BW=135MiB/s (142MB/s)(4053MiB/30001msec); 0 zone resets
[q=8,direct]  write: IOPS=110k, BW=432MiB/s (452MB/s)(12.6GiB/30001msec); 0 zone resets
[q=32,direct]  write: IOPS=125k, BW=490MiB/s (513MB/s)(14.3GiB/30001msec); 0 zone resets
[q=32,direct]  write: IOPS=134k, BW=525MiB/s (550MB/s)(15.4GiB/30001msec); 0 zone resets
```

so it looks like libaio direct is probably the right way to go.

buffered is pretty good:
```
[q=32,buf]  write: IOPS=151k, BW=590MiB/s (619MB/s)(17.3GiB/30001msec); 0 zone resets
[q=32,buf]  write: IOPS=218k, BW=851MiB/s (892MB/s)(24.9GiB/30001msec); 0 zone resets
```
