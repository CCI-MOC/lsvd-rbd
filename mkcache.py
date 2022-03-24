#!/usr/bin/python3

import lsvd
import sys
import os
import argparse
import uuid
import blkdev

# TODO:
# 1. variable size
# 3. option to not write cache pages

def div_round_up(n, m):
    return (n + m - 1) // m

def mkcache(name, uuid=b'\0'*16, write_zeros=True, wblks=125, rblks=16*16):
    fd = os.open(name, os.O_RDWR | os.O_CREAT, 0o777)

    sup = lsvd.j_super(magic=lsvd.LSVD_MAGIC, type=lsvd.LSVD_J_SUPER,
                       write_super=1, read_super=2, backend_type=lsvd.LSVD_BE_FILE)
    sup.vol_uuid[:] = uuid
    data = bytearray() + sup
    data += b'\0' * (4096-len(data))
    os.write(fd, data) # page 0

    # assuming 4KB min write size, write cache has max metadata of 12 bytes
    # extent + 8 bytes length per 8KB (header + block), but we need an integer
    # number of pages for each section, and enough room for 2.
    #
    _map = div_round_up(wblks, 600)
    _len = div_round_up(wblks, 1024)
    mblks = 2 * (_map + _len)
    wblks -= mblks
    
    # write cache has 125 single-page entries. default: map_blocks = map_entries = 0
    wsup = lsvd.j_write_super(magic=lsvd.LSVD_MAGIC, type=lsvd.LSVD_J_W_SUPER,
                              seq=1, meta_base = 3, meta_limit = 3+mblks,
                              base=3+mblks, limit=3+mblks+wblks,
                              next=3+mblks, oldest=3+mblks)
    wsup.vol_uuid[:] = uuid
    data = bytearray() + wsup
    data += b'\0' * (4096-len(data))
    os.write(fd, data) # page 1

    rbase = wblks+mblks+3
    units = rblks // 16
    map_blks = div_round_up(units*lsvd.sizeof_obj_offset, 4096)
    bitmap_blks = div_round_up(units*2, 4096)
    
    # 1 page for map, 1 page for bitmap
    rsup = lsvd.j_read_super(magic=lsvd.LSVD_MAGIC, type=lsvd.LSVD_J_R_SUPER,
                                unit_size=128, units=units,
                                map_start=rbase, map_blocks=map_blks,
                                bitmap_start=rbase+map_blks, bitmap_blocks=bitmap_blks,
                                base=rbase+map_blks+bitmap_blks)

    rsup.vol_uuid[:] = uuid
    data = bytearray() + rsup
    data += b'\0' * (4096-len(data))
    os.write(fd, data) # page 2

    # zeros are invalid entries for cache map/bitmap
    # 130 + 16*(64KB/4KB) = 386

    if (write_zeros):
        data = bytearray(b'\0'*4096)
        for i in range(3 + mblks + wblks + map_blks + bitmap_blks + rblks):
            os.write(fd, data)
    os.close(fd)
    
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='initialize LSVD cache')
    parser.add_argument('--uuid', help='volume UUID',
                            default='00000000-0000-0000-0000-000000000000')
    parser.add_argument('device', help='cache partition')
    args = parser.parse_args()

    uuid = uuid.UUID(args.uuid).bytes

    if os.access(args.device, os.F_OK):
        s = os.stat(args.device)
        if blkdev.S_ISBLK(s.st_mode):
            bytes = blkdev.dev_get_size(os.open(args.device, os.O_RDONLY))
        else:
            bytes = s.st_size
        pages = bytes // 4096
        print('%d pages' % pages)
        r_units = int(0.75*pages) // 16
        r_pages = r_units * 16
        r_oh = div_round_up(r_units*(2+lsvd.sizeof_obj_offset), 4096)
        w_pages = pages - r_pages - 3     # mkcache subtracts write metadata
    
        mkcache(args.device, uuid, write_zeros=False, wblks=w_pages, rblks=r_pages)
    else:
        mkcache(args.device, uuid)

