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
    nbytes = len(data)
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    val = lsvd_lib.c_write(bytes(data), c_ulong(offset), c_uint(nbytes), c_ulong(0))
    return val

def read(offset, nbytes):
    assert (nbytes % 512) == 0 and (offset % 512) == 0
    buf = (c_char * len)()
    val = lsvd_lib.c_read(buf, c_ulong(offset), c_uint(nbytes), c_ulong(0))
    return val

def getmap(base, limit):
    n_tuples = 128
    tuples = (tuple * n_tuples)()
    n = lsvd_lib.dbg_getmap(c_int(base), c_int(limit), c_int(n_tuples), byref(tuples))
    return n, tuples[0:n]

def inmem():
    n_objs = 32
    objs = (c_int * n_objs)()
    n = lsvd_lib.dbg_inmem(c_int(n_objs), byref(objs))
    return map(lambda x: [x.base, x.limit, x.obj, x.offset], objs)
