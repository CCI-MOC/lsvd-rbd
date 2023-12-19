# How to launch VMs on QEMU with LSVD

There's an unattended ubuntu install config on `seed.iso`, so to use that as the
install config, run QEMU with 
`-drive format=raw,file=seed.iso,cache=none,if=virtio`.

1. Create a lsvd image if you don't already have one: 
   `./imgtool --create --rados --size 10g $pool_name/$img_name`
2. Launch LSVD as a NVMF target `qemu-gateway.bash $pool_name $img_name`
3. Lanuch QEMU with the NVMF target `qemu-client.bash`. This does the following:
   - `nvme connect` to the nvmf target on the gateway
   - Launch QEMU with the mounted NVMF target as the drive

For alternative client configurations, comment out the qemu launch command in
`qemu-client.bash`, and use one of the following:

To install Ubuntu 2204, use:

```
qemu-system-x86_64 \
   -enable-kvm \
   -m 1024 \
   -cdrom <path to ubuntu install iso> \
   -vnc :1 -serial mon:stdio \
   -drive format=raw,file=seed.iso,cache=none,if=virtio \
   -drive format=raw,file=<device where you want ubuntu installed>
```

To run a VM with an existing image, use:

```
qemu-system-x86_64 \
	-enable-kvm \
	-m 1024 \
	-vnc :1 -serial mon:stdio \
	-hda $dev_name
```

For nographic, extract the kernel from the image and use:

```
qemu-system-x86_64 \
	-enable-kvm \
	-m 1G \
	-serial mon:stdio \
	-nographic \
	-kernel <path to kernel> \
	-hda $dev_name \
	-append "root=/dev/sda2 console=ttyS0"
```

