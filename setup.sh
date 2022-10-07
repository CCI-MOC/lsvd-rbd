python3 delete-rados.py rbd:obj_B
python3 mkdisk.py --rados --uuid 7cf1fca0-a182-11ec-8abf-37d9345adf42 --size 5g rbd/obj_B
python3 mkcache.py --uuid 7cf1fca0-a182-11ec-8abf-37d9345adf42 /mnt/nvme/lsvd/SSD
