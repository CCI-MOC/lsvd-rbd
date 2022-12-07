#!/usr/bin/python3

import ctypes
import lsvd_types as lsvd
import os
import re
import argparse
import uuid
try:
    import rados
except:
    pass
    
def read_obj(name):
    if args.rados:
        pool,oid = name.split('/')
        if not cluster.pool_exists(pool):
            raise RuntimeError('Pool not found: ' + pool)
        ioctx = cluster.open_ioctx(pool)
        obj = ioctx.read(oid)
        ioctx.close()
    else:
        fd = os.open(name, os.O_RDONLY)
        obj = os.read(fd, 64*1024)
        os.close(fd)
    return obj

def write_obj(name, data):
    if args.rados:
        pool,oid = name.split('/')
        if not cluster.pool_exists(pool):
            raise RuntimeError('Pool not found: ' + pool)
        ioctx = cluster.open_ioctx(pool)
        ioctx.write(oid, bytes(data))
        ioctx.close()
    else:
        fd = os.open(name, os.O_WRONLY|os.O_TRUNC|os.O_CREAT)
        os.write(fd, bytes(data))
        os.close(fd)

# we assume that the base volume is completely shut down, so the 
# highest-numbered object is the last checkpoint
#
def mk_clone(old, new, uu):
    obj = read_obj(old)
    o2 = lsvd.sizeof_hdr
    h = lsvd.hdr.from_buffer(bytearray(obj[0:o2]))

    o3 = o2+lsvd.sizeof_super_hdr
    sh = lsvd.super_hdr.from_buffer(bytearray(obj[o2:o3]))

    _ckpts = obj[sh.ckpts_offset:sh.ckpts_offset+sh.ckpts_len]
    ckpts = (ctypes.c_int * (len(_ckpts)//4)).from_buffer(bytearray(_ckpts))
    seq = ckpts[-1]
    
    h2 = lsvd.hdr(magic=lsvd.LSVD_MAGIC, version=1, type=lsvd.LSVD_SUPER,
                      hdr_sectors=8, data_sectors=0)
    h2.vol_uuid[:] = uu

    
    b_old = bytes(old,'utf-8')
    c = lsvd.clone(sequence=seq+1, name_len=len(b_old))
    c.vol_uuid[:] = h.vol_uuid[:]
    sizeof_clone = lsvd.sizeof_clone + len(b_old)
    offset_clone = lsvd.sizeof_hdr+lsvd.sizeof_super_hdr
    
    sh2 = lsvd.super_hdr(vol_size=sh.vol_size, next_obj=sh.next_obj,
                             clones_offset=offset_clone, clones_len=sizeof_clone)

    data = bytearray() + h2 + sh2 + c + b_old
    data += b'\0' * (4096-len(data))

    write_obj(new, data)
    
if __name__ == '__main__':
    _rnd_uuid = str(uuid.uuid4())
    parser = argparse.ArgumentParser(description='create LSVD disk')
    parser.add_argument('--uuid', help='volume UUID', default=_rnd_uuid)
    parser.add_argument('--rados', help='use RADOS backend', action='store_true');
    parser.add_argument('base', help='base image')
    parser.add_argument('image', help='new (clone) image')
    args = parser.parse_args()

    _uuid = uuid.UUID(args.uuid).bytes

    if args.rados:
        cluster = rados.Rados(conffile='')
        cluster.connect()

    mk_clone(args.base, args.image, _uuid)

    if args.rados:
        cluster.shutdown()
