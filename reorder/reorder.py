#!/usr/bin/python3
#
# first pass at parsing an image for reordering
# objects must be downloaded to a directory, takes the name
# of the superblock object (i.e. prefix) as argument, parses it
#

import zlib
import os
import lsvd_types as lsvd
import sys
import uuid
from ctypes import *
import argparse
import extent

# returns obj_info, map

def parse_ckpt(obj):
    o2 = l1 = lsvd.sizeof_hdr
    h = lsvd.hdr.from_buffer(bytearray(obj[0:l1]))
    if h.type != lsvd.LSVD_CKPT:
        return None
    
    o3 = o2+lsvd.sizeof_ckpt_hdr
    ch = lsvd.ckpt_hdr.from_buffer(bytearray(obj[o2:o3]))

    o4 = ch.ckpts_offset
    l4 = ch.ckpts_len
    ckpts = (c_uint * (l4//4)).from_buffer(bytearray(obj[o4:o4+l4]))

    o5 = ch.objs_offset
    l5 = ch.objs_len

    if o5+l5 > len(obj):
        return None

    objs = (lsvd.ckpt_obj * (l5//lsvd.sizeof_ckpt_obj)
                ).from_buffer(bytearray(obj[o5:o5+l5]))
    objs = [(_.seq, _.hdr_sectors,
                 _.data_sectors, _.live_sectors) for _ in objs]

    o7 = ch.map_offset
    l7 = ch.map_len
    if o7+l7 > len(obj):
        return None

    maps = (lsvd.ckpt_mapentry * (l7//lsvd.sizeof_ckpt_mapentry)
                ).from_buffer(bytearray(obj[o7:o7+l7]))
    maps = [(_.lba, _.len, _.obj, _.offset) for _ in maps]

    return objs,maps

            
def read_obj(name, sectors=0):
    fp = open(name, 'rb')
    if sectors > 0:
        data = fp.read(sectors*512)
    else:
        data = fp.read()
    fp.close()
    return data

def read_ckpt_list(buf, base, bytes):
    if bytes <= 0:
        return []
    n = bytes//4
    ckpts = (c_int * n).from_buffer(buf[base:base+bytes])
    return [_ for _ in ckpts]

def super_ckpts(name):
    obj = read_obj(name)
    o2 = l1 = lsvd.sizeof_hdr
    h = lsvd.hdr.from_buffer(bytearray(obj[0:l1]))
    if h.type != lsvd.LSVD_SUPER:
        return None
    o3 = o2+lsvd.sizeof_super_hdr
    sh = lsvd.super_hdr.from_buffer(bytearray(obj[o2:o3]))
    return read_ckpt_list(bytearray(obj), sh.ckpts_offset, sh.ckpts_len)

# file descriptor cache for reading data from objects
#
fp_map = dict()                           # filename -> open file
fp_que = []                               # for FIFO cache replacement
fp_max = 100                              # cached file descriptors

def get_fp(name):
    if name not in fp_map:
        if len(fp_map) >= fp_max:
            n = fp_que.pop(0)
            fp_map[n].close()
            del fp_map[n]
        fp_que.append(name)
        fp_map[name] = open(name, 'rb')
    return fp_map[name]

# find zero pages. Results are disappointing so far, not much
# opportunity for compression
#
zeros = bytearray(4096)
def is_zero(obj, offset, base):
    oname = '%s.%08x' % (prefix, obj)
    fp = get_fp(oname)
    fp.seek(offset*512, 0)
    data = fp.read(4096)
    return data == zeros

# read range from object
#
def get_bytes(obj, offset, sectors):
    oname = '%s.%08x' % (prefix, obj)
    fp = get_fp(oname)
    fp.seek(offset*512, 0)
    data = fp.read(sectors*512)
    return data

if __name__ == '__main__':
    prefix = sys.argv[1]
    c = super_ckpts(prefix)
    ckpt_name = '%s.%08x' % (prefix, c[-1])
    ckpt = read_obj(ckpt_name)
    objs, extents = parse_ckpt(ckpt)

    _map = extent.map()
    for _lba,_len,_obj,_offset in extents:
        _map.update(_lba, _lba+_len, _obj, _offset)

    print('map length:', _map.size())

    # this is what it would look like if we "linearized" the image
    #
    o = 1
    l,l_max = 0,16384

    # iterate over each extent in the map, assign it a location
    # 
    for e in _map:
        _base,_limit,_obj,_offset = e.vals()
        while _base < _limit:
            _len = min(_limit-_base, l_max - l)
            print(_obj, _offset, '+', _len, '->', o, l)
            data = get_bytes(_obj, _offset, _len)
            _base += _len
            _offset += _len
            l += _len
            if l >= l_max:
                l,o = 0,o+1
    print('linearized objects:', o)

    # sectors,nz_sectors = 0,0
    # for e in _map:
    #     _base,_limit,_obj,_offset = e.vals()
    #     if (_base % 8 != 0) or (_limit % 8 != 0):
    #         sectors += (_limit-_base)
    #         nz_sectors += (_limit-_base)
    #         continue
    #     for i in range(0, _limit-_base, 8):
    #         sectors += 8
    #         if is_zero(_obj, _offset+i, _base+i):
    #             _map.update(_base+i, _base+i+8, -1, 0)
    #         else:
    #             nz_sectors += 8
    # print('deduped len:', _map.size())
    # print('total sectors:   ', sectors)
    # print('non-zero sectors:', nz_sectors)
