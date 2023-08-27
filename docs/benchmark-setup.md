# Pools

There's a few types of pools on there:

# EC32 ssd pool

- Create the ec profile: `ceph osd erasure-code-profile set profile-ec32-ssd crush-root=ssd k=3 m=2 crush-failure-domain=osd`
- Create the ec rule: `ceph osd crush rule create-erasure rule-ec32-ssd`
- Create the pool: `ceph osd pool create ec32-ssd erasure profile-ec32-ssd rule-ec32-ssd`

# setup

- put cache on nvme, not sata

- 20G size
- 256 io depth
	- compare 32, 64, 128, 256
- rbd v lsvd
- ycsb and filebench (assuming ext4)
	- sync-heavy workloads: varmail, oltp,
- hdd v ssd backend
- compare block sizes
- empty image case (benefits sparse allocation)
- 1 fio job

- re-use experimental config from lsvd paper

workload configs

- 8-10 workloads?
- 4k block size, 1/128 queue depth,
- try out 8/16k block size
- fio workloads: seq read/write, rand read/write
- filebench (with ext4): fileserver, oltp, varmail

backend configs

- 4 backends
- compare ssd and hdd backend
- compare lsvd and rbd
- erasure coded if possible

disk configs

- from lsvd paper
- 80g disk size
- 4k block size

not doing for now:

- vm boot
