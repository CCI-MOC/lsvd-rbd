#!/usr/bin/python3

import ctypes
from lsvd_types import *
import os
import re
import argparse
import uuid
import rados

# based on https://stackoverflow.com/a/42865957/2002471
units = {"B": 1, "KB": 2**10, "MB": 2**20, "GB": 2**30, "TB": 2**40}

def parse_size(size):
    size = re.sub(r'(\d+)([KMGT]?)', r'\1 \2B', size.upper())
    number, unit = [string.strip() for string in size.split()]
    return int(float(number)*units[unit])

def mkdisk(name, sectors, uuid=b'\0'*16, use_rados=False):
    _hdr = hdr(magic=LSVD_MAGIC, version=1, type=LSVD_SUPER,
                  hdr_sectors=8, data_sectors=0)
    _hdr.vol_uuid[:] = uuid
    data = bytearray() + _hdr
    
    super = super_hdr(vol_size=sectors, next_obj=1)
    data += super

    data += b'\0' * (4096-len(data))

    if use_rados:
        cluster = rados.Rados(conffile='')
        cluster.connect();
        pool,prefix = name.split("/")
        if not cluster.pool_exists(pool):
            raise RuntimeError('Pool not found ' + pool)
        ioctx = cluster.open_ioctx(pool)
        print(type(data))
        ioctx.write(prefix, bytes(data))
        ioctx.close()
        cluster.shutdown()
    else:
        fp = open(name, 'wb')
        fp.write(data) # page 1
        fp.close()

def cleanup(name):
    d = os.path.dirname(name)
    b = os.path.basename(name)
    for f in os.listdir(d):
        if f.startswith(b):
            os.unlink(d + '/' + f)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='create LSVD disk')
    parser.add_argument('--size', help='volume size (k/m/g)', default='10m')
    parser.add_argument('--uuid', help='volume UUID',
                            default='00000000-0000-0000-0000-000000000000')
    parser.add_argument('--rados', help='use RADOS backend', action='store_true');
    parser.add_argument('prefix', help='superblock name')
    args = parser.parse_args()

    size = parse_size(args.size)
    _uuid = uuid.UUID(args.uuid).bytes
    if _uuid == b'\0'*16:
        _uuid = uuid.uuid1().bytes

    if not args.rados:
        cleanup(args.prefix)
    mkdisk(args.prefix, size//512, _uuid, args.rados)

