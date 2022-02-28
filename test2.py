#
# basic unit tests 
#

import unittest
import lsvd
import os
import ctypes

def blank_uuid():
    return (ctypes.c_ubyte*16)()

def write_data_1(name, ckpt, seq):
    data = bytearray()
    chars = [_ for _ in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ']
    for _ in range(79):
        for c in chars:
            data += (c * 512)
    data = data[0:1024*1024]
    data_sectors = len(data) / 512
    
    map_entries = [[_, 1] for _ in range(0, 4096, 2)]
    map_len = lsvd.sizeof_data_map * len(map_entries)
    map_buf = bytearray()
    for lba, sectors in map_entries:
        map_buf += lsvd.data_map(lba, sectors)
    hdr_bytes = len(map_buf) + lsvd.sizeof_hdr + lsvd.sizeof_data_hdr
    if ckpt:
        hdr_bytes += 4
    hdr_sectors = (hdr_bytes + 511) // 512

    h = lsvd.hdr(lsvd.LSVD_MAGIC, 1, blank_uuid(), lsvd.LSVD_DATA,
                     seq, hdr_sectors, data_sectors)

    offset = lsvd.sizeof_hdr + lsvd.sizeof_data_hdr
    ckpt_len = 4 if ckpt else 0
    dh = lsvd.data_hdr(0,                 # last data object
                       offset,            # checkpoints offset
                       ckpt_len,          # 0 or 1 checkpoint
                       0, 0,              # objects cleaned
                       offset + ckpt_len, # map offset
                       len(_map))

    hdr = bytearray()
    hdr += h
    hdr += dh
    if ckpt:
        hdr += ctypes.c_uint(ckpt)
    hdr += map_buf
    pad = hdr_sectors*512 - len(hdr)
    hdr += b'\0' * pad

    fp = open(name, 'wb')
    fp.write(hdr)
    fp.write(data)
    fp.close()

# if ckpt > 0, add it
def write_super(name, ckpt, next_obj):
    h = lsvd.hdr(lsvd.LSVD_MAGIC, 1, blank_uuid(), lsvd.LSVD_SUPER, 0, 8, 0)
    #                             ver                     sectors: seq hdr data
    size = 10*1024*1024 / 512 # 10MB
    offset = lsvd.sizeof_hdr + lsvd.sizeof_super_hdr
    ckpt_len = 4 if ckpt else 0
    
    sh = lsvd.super_hdr(size,
                        0,                # total sectors
                        0,                # live sectors
                        next_obj,
                        offset,           # ckpts_offset
                        ckpt_len,
                        0, 0,             # clone bases: offset, len
                        0, 0)             # snapshots: offset, len

    ckpt = ctypes.c_uint(ckpt)            # ignored if len=0
    buf = bytearray() + h + sh + ckpt
    buf += (b'\0' * (4096 - len(buf)))

    fp = open(name, 'wb')
    fp.write(buf)
    fp.close()
    

class tests(unittest.TestCase):
    def test_1_size(self):
        self.assertTrue(_size == 10*1024*1024)
        
        
from time import sleep
if __name__ == '__main__':
    unittest.main(exit=False)
    sleep(1)
    
