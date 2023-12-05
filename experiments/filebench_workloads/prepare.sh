sudo umount /mnt_benchmarks/
mkfs.ext4 /dev/dm-0
sudo mount /dev/dm-0 /mnt_benchmarks/
echo 0 > /proc/sys/kernel/randomize_va_space
#mkdir /mnt_benchmarks/200k /mnt_benchmarks/800k /mnt_benchmarks/50k