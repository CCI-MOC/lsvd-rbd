#!/usr/bin/python3

import ctypes
import lsvd
import os
import re
import argparse
import uuid

# based on https://stackoverflow.com/a/42865957/2002471
units = {"B": 1, "KB": 2**10, "MB": 2**20, "GB": 2**30, "TB": 2**40}

def parse_size(size):
    size = re.sub(r'(\d+)([KMGT]?)', r'\1 \2B', size.upper())
    number, unit = [string.strip() for string in size.split()]
    return int(float(number)*units[unit])

def mkdisk(name, sectors, uuid=b'\0'*16):
    fp = open(name, 'wb')

    hdr = lsvd.hdr(magic=lsvd.LSVD_MAGIC, version=1, type=lsvd.LSVD_SUPER,
                       hdr_sectors=8, data_sectors=0)
    hdr.vol_uuid[:] = uuid
    data = bytearray() + hdr
    
    super = lsvd.super_hdr(vol_size=sectors, next_obj=1)
    data += super

    data += b'\0' * (4096-len(data))
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
    parser.add_argument('prefix', help='superblock name')
    args = parser.parse_args()

    size = parse_size(args.size)
    uuid = uuid.UUID(args.uuid).bytes
    cleanup(args.prefix)
    mkdisk(args.prefix, size//512, uuid)

