#!/usr/bin/python3

import lsvd
import sys
import os
import argparse
import uuid

# TODO:
# 1. variable size
# 3. option to not write cache pages

def mkcache(name, uuid=b'\0'*16):
    fp = open(name, 'wb')

    sup = lsvd.j_super(magic=lsvd.LSVD_MAGIC, type=lsvd.LSVD_J_SUPER,
                       write_super=1, read_super=2, backend_type=lsvd.LSVD_BE_FILE)
    sup.vol_uuid[:] = uuid
    data = bytearray() + sup
    data += b'\0' * (4096-len(data))
    fp.write(data) # page 0

    # write cache has 125 single-page entries. default: map_blocks = map_entries = 0
    wsup = lsvd.j_write_super(magic=lsvd.LSVD_MAGIC, type=lsvd.LSVD_J_W_SUPER,
                              seq=1, base=3, limit=128, next=3, oldest=127)
    wsup.vol_uuid[:] = uuid
    data = bytearray() + wsup
    data += b'\0' * (4096-len(data))
    fp.write(data) # page 1

    # 1 page for map, 1 page for bitmap
    rsup = lsvd.j_read_super(magic=lsvd.LSVD_MAGIC, type=lsvd.LSVD_J_R_SUPER,
                                unit_size=128, base=130, units=16, map_start=128,
                                map_blocks=1, bitmap_start=129, bitmap_blocks=1)
    rsup.vol_uuid[:] = uuid
    data = bytearray() + rsup
    data += b'\0' * (4096-len(data))
    fp.write(data) # page 2

    # zeros are invalid entries for cache map/bitmap
    # 130 + 16*(64KB/4KB) = 386
    
    data = bytearray(b'\0'*4096)
    for i in range(2,386):
        fp.write(data)
    fp.close()
    
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='initialize LSVD cache')
    parser.add_argument('--uuid', help='volume UUID',
                            default='00000000-0000-0000-0000-000000000000')
    parser.add_argument('device', help='cache partition')
    args = parser.parse_args()

    uuid = uuid.UUID(args.uuid).bytes
    mkcache(args.device, uuid)

