#!/usr/bin/python3

import os
import lsvd
import sys
from ctypes import *

f = open(sys.argv[1], 'rb')
obj = f.read(None)
f.close()

o2 = l1 = lsvd.sizeof_hdr

def fmt_ckpt(ckpts):
    return map(lambda x: "%d" % x, ckpts)

def fmt_obj_cleaned(objs):
    l = []
    for o in objs:
        l.append("[%d %d]" % (o.seq, o.was_deleted))
    return l

def fmt_data_map(maps):
    l = []
    for m in maps:
        l.append("[%d %d]" % (m.lba, m.len))
    return l

h = lsvd.hdr.from_buffer(bytearray(obj[0:l1]))
if h.type == lsvd.LSVD_SUPER:
    o3 = o2+lsvd.sizeof_super_hdr
elif h.type == lsvd.LSVD_DATA:
    o3 = o2+lsvd.sizeof_data_hdr
    dh = lsvd.data_hdr.from_buffer(bytearray(obj[o2:o3]))
    
    o4 = dh.ckpts_offset; l4 = dh.ckpts_len
    ckpts = (c_uint * (l4//4)).from_buffer(bytearray(obj[o4:o4+l4]))
    
    o5 = dh.objs_cleaned_offset; l5 = dh.objs_cleaned_len
    objs = (lsvd.obj_cleaned * (l5//lsvd.sizeof_obj_cleaned)).from_buffer(bytearray(obj[o5:o5+l5]))

    o6 = dh.map_offset; l6 = dh.map_len
    maps = (lsvd.data_map * (l6//lsvd.sizeof_data_map)).from_buffer(bytearray(obj[o6:o6+l6]))

    print('name:     ', sys.argv[1])
    print('magic:    ', 'OK' if h.magic == lsvd.LSVD_MAGIC else '**BAD**')
    print('version:  ', h.version)
    print('type:     ', 'DATA')
    print('seq:      ', h.seq)
    print('n_hdr:    ', h.hdr_sectors)
    print('n_data:   ', h.data_sectors)

    print('last_data:', dh.last_data_obj)
    print('ckpts:    ', dh.ckpts_offset, ':', ', '.join(fmt_ckpt(ckpts)))
    print('cleaned:  ', dh.objs_cleaned_offset, ':', ', '.join(fmt_obj_cleaned(objs)))
    print('map:      ', dh.map_offset, ':', ', '.join(fmt_data_map(maps)))
    
elif h.type == lsvd.LSVD_CKPT:
    o3 = o2+lsvd.sizeof_ckpt_hdr
else:
    print("invalid type:", h.type)
