#!/usr/bin/python3

import lsvd
import sys
import os

# TODO:
# 1. variable size
# 2. read cache
# 3. option to not write cache pages

def mkcache(name):
    fp = open(name, 'wb')

    sup = lsvd.j_super(magic=lsvd.LSVD_MAGIC, type=lsvd.LSVD_J_SUPER,
                       write_super=1, read_super=2, backend_type=lsvd.LSVD_BE_FILE)
    data = bytearray() + sup
    data += b'\0' * (4096-len(data))
    fp.write(data)

    wsup = lsvd.j_write_super(magic=lsvd.LSVD_MAGIC, type=lsvd.LSVD_J_W_SUPER,
                              read_super=2, seq=1, base=3, limit=127, next=3, oldest=127)
    data = bytearray() + wsup
    data += b'\0' * (4096-len(data))
    fp.write(data)

    data = bytearray(b'\0'*4096)
    for i in range(2,128):
        fp.write(data)
    fp.close()
    
if __name__ == '__main__':
    mkcache(sys.argv[1])

