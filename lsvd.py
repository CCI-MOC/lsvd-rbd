from ctypes import *
import os

class tuple(Structure):
    _fields_ = [("base", c_int),
                ("limit", c_int),
                ("obj", c_int),
                ("offset", c_int)]

dir = os.getcwd()
lsvd_lib = CDLL(dir + "/liblsvd.so")
assert lsvd_lib

def write(offset, data):
    if type(data) != bytes:
        data = bytes(data, 'utf-8')
    nbytes = len(data)
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    val = lsvd_lib.c_write(data, c_ulong(offset), c_uint(nbytes), c_ulong(0))
    return val

def read(offset, nbytes):
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    buf = (c_char * nbytes)()
    val = lsvd_lib.c_read(buf, c_ulong(offset), c_uint(nbytes), c_ulong(0))
    return buf[0:nbytes]

def getmap(base, limit):
    n_tuples = 128
    tuples = (tuple * n_tuples)()
    n = lsvd_lib.dbg_getmap(c_int(base), c_int(limit), c_int(n_tuples), byref(tuples))
    return [_ for _ in map(lambda x: [x.base, x.limit, x.obj, x.offset], tuples[0:n])]

def inmem():
    n_objs = 32
    objs = (c_int * n_objs)()
    n = lsvd_lib.dbg_inmem(c_int(n_objs), byref(objs))
    return objs[0:n]

def flush():
    return lsvd_lib.c_flush(c_ulong(0))

def init(name, n):
    if type(name) != bytes:
        name = bytes(name, 'utf-8')
    return lsvd_lib.c_init(name, c_int(n))

def size():
    return lsvd_lib.c_size()

def shutdown():
    lsvd_lib.c_shutdown()

def batch_seq():
    return c_int.in_dll(lsvd_lib, 'batch_seq').value

def checkpoint():
    return lsvd_lib.dbg_checkpoint()

LSVD_SUPER = 1
LSVD_DATA = 2
LSVD_CKPT = 3
LSVD_MAGIC = 0x4456534c

# these match version 453d93 of objects.cc

class hdr(Structure):
    _fields_ = [("magic",               c_uint),
                ("version",             c_uint),
                ("vol_uuid",            c_ubyte*16),
                ("type",                c_uint),
                ("seq",                 c_uint),
                ("hdr_sectors",         c_uint),
                ("data_sectors",        c_uint)]
sizeof_hdr = sizeof(hdr) # 40

class super_hdr(Structure):
    _fields_ = [("vol_size",            c_ulong),
                ("total_sectors",       c_ulong),
                ("live_sectors",        c_ulong),
                ("next_obj",            c_uint),
                ("ckpts_offset",        c_uint),
                ("ckpts_len",           c_uint),
                ("clones_offset",       c_uint),
                ("clones_len",          c_uint),
                ("snaps_offset",        c_uint),
                ("snaps_len",           c_uint)]
sizeof_super_hdr = sizeof(super_hdr) # 56

# ckpts is array of uint32

class clone(Structure):
    _pack_ = 1
    _fields_ = [("vol_uuid",            c_ubyte*16),
                ("sequence",            c_uint),
                ("name_len",            c_ubyte)]   # 21 bytes
        # followed by 'name_len' bytes of name, use string_at
sizeof_clone = sizeof(clone) # 21

class snap(Structure):
    _fields_ = [("snap_uuid",           c_ubyte*16),
                ("seq",                 c_uint)]
sizeof_snap = sizeof(snap) # 20

class data_hdr(Structure):
    _fields_ = [("last_data_obj",       c_uint),
                ("ckpts_offset",        c_uint),
                ("ckpts_len",           c_uint),
                ("objs_cleaned_offset", c_uint),
                ("objs_cleaned_len",    c_uint),
                ("map_offset",          c_uint),
                ("map_len",             c_uint)]
sizeof_data_hdr = sizeof(data_hdr) # 28

class obj_cleaned(Structure):
    _fields_ = [("seq",                 c_uint),
                ("was_deleted",         c_uint)]
sizeof_obj_cleaned = sizeof(obj_cleaned) # 8

class data_map(LittleEndianStructure):
    _fields_ = [("lba",                 c_ulong, 36),
                ("len",                 c_ulong, 28)]
sizeof_data_map = sizeof(data_map) # 8

class ckpt_hdr(Structure):
    _fields_ = [("ckpts_offset",        c_uint),
                ("ckpts_len",           c_uint),
                ("objs_offset",         c_uint),
                ("objs_len",            c_uint),
                ("deletes_offset",      c_uint),
                ("deletes_len",         c_uint),
                ("map_offset",          c_uint),
                ("map_len",             c_uint)]
sizeof_ckpt_hdr = sizeof(ckpt_hdr) # 32

class ckpt_obj(Structure):
    _fields_ = [("seq",                 c_uint),
                ("hdr_sectors",         c_uint),
                ("data_sectors",        c_uint),
                ("live_sectors",        c_uint)]
sizeof_ckpt_obj = sizeof(ckpt_obj) # 16

class deferred_delete(Structure):
    _fields_ = [("seq",                 c_uint),
                ("time",                c_uint)]
sizeof_deferred_delete = sizeof(deferred_delete)

class ckpt_mapentry(LittleEndianStructure):
    _fields_ = [("lba",                 c_ulong, 36),
                ("len",                 c_ulong, 28),
                ("obj",                 c_uint),
                ("offset",              c_uint)]
sizeof_ckpt_mapentry = sizeof(ckpt_mapentry) # 16

