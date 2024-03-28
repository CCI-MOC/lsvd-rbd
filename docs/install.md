# SPDK setup

- Clone the entire repo including submodules
- Build SPDK with RBD support

```
./configure --with-rbd
make
```
- Build LSVD: `make release`
- Run scripts are available in `qemu/` or `experiments/` directories