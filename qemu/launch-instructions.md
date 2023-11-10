# How to launch VMs on QEMU with LSVD

1. Get your hands on a raw image of an ubuntu install. Any will do. There's one
   on `/home/isaackhor/ubuntu2204.img` on `dl380p-6`
2. Create a lsvd image `./imgtool --create --rados --size 10g $pool_name/$img_name`
3. Launch LSVD as a NVMF target `qemu-gateway.bash`
4. Launch QEMU with the mounted NVMF target `qemu-client.bash`
