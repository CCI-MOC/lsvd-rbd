from ctypes import *
import os

class tuple(Structure):
    _fields_ = [("base",   c_int),
                ("limit",  c_int),
                ("obj",    c_int),
                ("offset", c_int),
                ("plba",   c_int)]

dir = os.getcwd()
lsvd_lib = CDLL(dir + "/liblsvd.so")
assert lsvd_lib

def write(offset, data):
    if type(data) != bytes:
        data = bytes(data, 'utf-8')
    nbytes = len(data)
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    val = lsvd_lib.c_write(data, c_ulong(offset), c_uint(nbytes))
    return val

def read(offset, nbytes):
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    buf = (c_char * nbytes)()
    val = lsvd_lib.c_read(buf, c_ulong(offset), c_uint(nbytes))
    return buf[0:nbytes]

def getmap(base, limit):
    n_tuples = 128
    tuples = (tuple * n_tuples)()
    n = lsvd_lib.dbg_getmap(c_int(base), c_int(limit), c_int(n_tuples), byref(tuples))
    return [_ for _ in map(lambda x: [x.base, x.limit, x.obj, x.offset], tuples[0:n])]

def frontier():
    return lsvd_lib.dbg_frontier()

def inmem():
    n_objs = 32
    objs = (c_int * n_objs)()
    n = lsvd_lib.dbg_inmem(c_int(n_objs), byref(objs))
    return objs[0:n]

def flush():
    return lsvd_lib.c_flush()

def init(name, n, flush):
    if type(name) != bytes:
        name = bytes(name, 'utf-8')
    return lsvd_lib.c_init(name, c_int(n), c_bool(flush), c_bool(True)) # nocache=T

def size():
    return lsvd_lib.c_size()

def shutdown():
    lsvd_lib.c_shutdown()

def batch_seq():
    return c_int.in_dll(lsvd_lib, 'batch_seq').value

def checkpoint():
    return lsvd_lib.dbg_checkpoint()

def reset():
    lsvd_lib.dbg_reset()

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

###############

_fd, fd = -1, -1
super = None

def cache_open(file):
    global _fd, fd, super
    _fd = os.open(file, os.O_RDWR | os.O_DIRECT)
    fd = os.open(file, os.O_RDONLY)
    data = os.pread(fd, 4096, 0)
    super = j_super.from_buffer(bytearray(data[0:sizeof(j_super)]))

def cache_close():
    os.close(_fd)
    os.close(fd)
    
def wcache_init(blkno):
    assert _fd != -1
    lsvd_lib.wcache_init(blkno, _fd)

def wcache_shutdown():
    lsvd_lib.wcache_shutdown()

def wcache_write(offset, data):
    if type(data) != bytes:
        data = bytes(data, 'utf-8')
    nbytes = len(data)
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    val = lsvd_lib.wcache_write(data, c_ulong(offset), c_uint(nbytes))

def wcache_read(offset, nbytes):
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    buf = (c_char * nbytes)()
    lsvd_lib.wcache_read(buf, c_ulong(offset), c_uint(nbytes))
    return buf[0:nbytes]
    
def wcache_reset():
    lsvd_lib.wcache_reset()

def wcache_getmap(base, limit):
    n_tuples = 128
    tuples = (tuple * n_tuples)()
    n = lsvd_lib.wcache_getmap(c_int(base), c_int(limit), c_int(n_tuples), byref(tuples))
    return list(map(lambda x: [x.base, x.limit, x.plba], tuples[0:n]))

def wcache_getsuper():
    s = j_write_super()
    lsvd_lib.wcache_get_super(byref(s))
    return s

def wcache_oldest(blk):
    exts = (j_extent * 32)()
    n = c_int()
    newer = lsvd_lib.wcache_oldest(c_int(blk), byref(exts), 32, byref(n))
    e = [(_.lba, _.len) for _ in exts[0:n.value]]
    return [newer, e]

def wcache_checkpoint():
    lsvd_lib.wcache_write_ckpt()

#----------------
class obj_offset(LittleEndianStructure):
    _fields_ = [("obj", c_ulong, 36),
                ("offset", c_ulong, 28)]
sizeof_obj_offset = sizeof(obj_offset)

rsuper = None
def rcache_init(blkno):
    global rsuper
    assert _fd != -1
    lsvd_lib.rcache_init(c_uint(blkno), c_int(_fd))
    rsuper = j_read_super()
    lsvd_lib.rcache_getsuper(byref(rsuper))

def rcache_shutdown():
    lsvd_lib.rcache_shutdown()

def rcache_read(offset, nbytes):
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    buf = (c_char * nbytes)()
    lsvd_lib.rcache_read(buf, c_ulong(offset), c_ulong(nbytes))
    return buf[0:nbytes]

def rcache_add(obj, offset, data):
    nbytes = len(data)
    assert (nbytes % 512) == 0
    lsvd_lib.rcache_add(c_int(obj), c_int(offset), data, c_int(nbytes), data)
    
def rcache_getmap():
    n = rsuper.units
    k = (obj_offset * n)()
    v = (c_int * n)()
    m = lsvd_lib.rcache_getmap(byref(k), byref(v), n)
    keys = [[_.obj, _.offset] for _ in k[0:m]]
    vals = v[:]
    return list(zip(keys, vals))
    
def rcache_flatmap():
    n = rsuper.units
    vals = (obj_offset * n)()
    n = lsvd_lib.rcache_get_flat(byref(vals), c_int(n))
    return [[_.obj,_.offset] for _ in vals[0:n]]

def rcache_bitmap():
    n = rsuper.units
    vals = (c_ushort * n)()
    n = lsvd_lib.rcache_get_masks(byref(vals), c_int(n))
    return vals[0:n]

def rcache_evict(n):
    lsvd_lib.rcache_evict(c_int(n))

def rcache_reset():
    lsvd_lib.rcache_reset()

# these manipulate the objmap directly, without going through the translation layer
#
def fakemap_update(base, limit, obj, offset):
    lsvd_lib.fakemap_update(c_int(base), c_int(limit), c_int(obj), c_int(offset))

def fakemap_reset():
    lsvd_lib.fakemap_reset()

# fake RBD functions
def fake_rbd_init():
    lsvd_lib.fake_rbd_init();

def fake_rbd_read(off, nbytes):
    buf = (c_char * nbytes)()
    lsvd_lib.fake_rbd_read(buf, c_ulong(off), c_ulong(nbytes))
    return buf[0:nbytes]

def fake_rbd_write(off, data):
    if type(data) != bytes:
        data = bytes(data, 'utf-8')
    nbytes = len(data)
    lsvd_lib.fake_rbd_write(data, c_ulong(off), c_ulong(nbytes))

def rbd_open(name):
    img = c_void_p()
    rv = lsvd_lib.rbd_open(None, c_char_p(bytes(name, 'utf-8')), byref(img), None)
    assert rv >= 0
    return img

def rbd_close(img):
    lsvd_lib.rbd_close(img)

def rbd_read(img, off, nbytes):
    buf = (c_char * nbytes)()
    lsvd_lib.rbd_read(img, c_ulong(off), c_ulong(nbytes), buf)
    return buf[0:nbytes]

def rbd_write(img, off, data):
    if type(data) != bytes:
        data = bytes(data, 'utf-8')
    nbytes = len(data)
    lsvd_lib.rbd_write(img, c_ulong(off), c_ulong(nbytes), data)
    
def rbd_flush(img):
    lsvd_lib.rbd_flush(img)

class j_extent(LittleEndianStructure):
    _fields_ = [("lba", c_ulong, 40),
                ("len", c_ulong, 24)]
sizeof_j_extent = sizeof(j_extent)

class j_map_extent(LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("lba", c_ulong, 40),
                ("len", c_ulong, 24),
                ("page", c_uint)]
sizeof_j_map_extent = sizeof(j_map_extent)

LSVD_J_DATA    = 10
LSVD_J_CKPT    = 11
LSVD_J_PAD     = 12
LSVD_J_SUPER   = 13
LSVD_J_W_SUPER = 14
LSVD_J_R_SUPER = 15

class j_hdr(Structure):
    _fields_ = [("magic",         c_uint),
                ("type",          c_uint),
                ("version",       c_uint),
                ("vol_uuid",      c_ubyte*16),
                ("seq",           c_ulong),
                ("len",           c_uint),
                ("crc32",         c_uint),
                ("extent_offset", c_uint),
                ("extent_len",    c_uint)]
sizeof_j_hdr = sizeof(j_hdr)

class j_write_super(Structure):
    _fields_ = [("magic",       c_uint),
                ("type",        c_uint),
                ("version",     c_uint),
                ("vol_uuid",    c_ubyte*16),
                ("seq",         c_ulong),
                ("base",        c_uint),
                ("limit",       c_uint),
                ("next",        c_uint),
                ("oldest",      c_uint),
                ("map_start",   c_uint),
                ("map_blocks",  c_uint),
                ("map_entries", c_uint)]
sizeof_j_write_super = sizeof(j_write_super)

class j_read_super(Structure):
    _fields_ = [("magic",        c_uint),
                ("type",         c_uint),
                ("version",      c_uint),
                ("vol_uuid",     c_ubyte*16),
                ("unit_size",    c_int),
                ("base",         c_int),
                ("units",        c_int),
                ("map_start",    c_int),
                ("map_blocks",   c_int),
                ("bitmap_start", c_int),
                ("bitmap_blocks", c_int),
                ("evict_type",   c_int),
                ("evict_start",  c_int),
                ("evict_blocks", c_int)]
sizeof_j_read_super = sizeof(j_read_super)

class j_super(Structure):
    _fields_ = [("magic",        c_uint),
                ("type",         c_uint),
                ("version",      c_uint),
                ("write_super",  c_uint),
                ("read_super",   c_uint),
                ("vol_uuid",     c_ubyte*16),
                ("backend_type", c_uint)]
sizeof_j_super = sizeof(j_super)

LSVD_BE_FILE  = 20
LSVD_BE_S3    = 21
LSVD_BE_RADOS = 22

class j_be_file(Structure):
    _fields_ = [("len", c_ushort),
                ("prefix", c_char*24)]      # hack. max of 24 bytes


