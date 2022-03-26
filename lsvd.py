from ctypes import *
import os
from lsvd_types import *

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
    return lsvd_lib.c_init(name, c_int(n), c_bool(flush))

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


