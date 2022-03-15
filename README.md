# lsvd-rbd
# Log Structured Virtual Disk

Described in the paper *Beating the I/O Bottleneck: A Case for Log-Structured Virtual Disks*, by Mohammad Hajkazemi, Vojtech Aschenbrenner, Mania Abdi, Emine Ugur Kaynar, Amin Mossayebzadeh, Orran Krieger, and Peter Desnoyers; EuroSys 2022

Uses two components: a local SSD partition (or file) and a remote backend store. (RADOS or S3)

The debug version uses files in a directory rather than an object store.



