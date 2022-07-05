rm /mnt/nvme/lsvd/*
dd if=/dev/zero bs=1024k count=100 of=/mnt/nvme/lsvd/SSD status=none
python3 mkdisk.py --uuid 7cf1fca0-a182-11ec-8abf-37d9345adf42 --size 1g /mnt/nvme/lsvd/obj
python3 mkcache.py --uuid 7cf1fca0-a182-11ec-8abf-37d9345adf42 /mnt/nvme/lsvd/SSD

