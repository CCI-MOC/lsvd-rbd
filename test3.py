#!/usr/bin/python3

import unittest
import lsvd
import os
import ctypes
import time
import mkcache

nvme = '/tmp/nvme'
img = '/tmp/bkt/obj'
dir = os.path.dirname(img)
fd2 = -1

def startup():
    global fd2
    val = os.system("./mkdisk --size 10m %s" % img)
    assert val == 0
    lsvd.init(img, 1, False)
    mkcache.mkcache(nvme)
    lsvd.cache_init(1, nvme)
    fd2 = os.open(nvme, os.O_RDONLY)

def c_super(fd):
    b = bytearray(os.pread(fd, 4096, 0))   # always first block
    return lsvd.j_super.from_buffer(b[0:lsvd.sizeof_j_super])

def c_w_super(fd, blk):
    b = bytearray(os.pread(fd, 4096, 4096*blk))
    return lsvd.j_write_super.from_buffer(b[0:lsvd.sizeof_j_write_super])

def c_r_super(fd, blk):
    b = bytearray(os.pread(fd, 4096, 4096*blk))
    return lsvd.j_read_super.from_buffer(b[0:lsvd.sizeof_j_read_super])

def c_hdr(fd, blk):
    b = bytearray(os.pread(fd, 4096, 4096*blk))
    o2 = lsvd.sizeof_j_hdr
    h = lsvd.j_hdr.from_buffer(b[0:o2])
    n_exts = h.extent_len // lsvd.sizeof_j_extent
    e = (lsvd.j_extent*n_exts).from_buffer(b[o2:o2+h.extent_len])
    return [h, e]

class tests(unittest.TestCase):
    #def setUp(self):
    #def tearDown(self):

    def test_1_readwrite(self):
        lsvd.cache_write(0, b'X'*4096)
        m = lsvd.cache_getmap(0, 1000)
        # header is block 3, data starts on 4
        self.assertEqual(m, [[0,8,(4*8)]])
        d = lsvd.cache_read(0, 4096)
        self.assertEqual(d, b'X'*4096)
        d = lsvd.cache_read(4096, 4096)
        self.assertEqual(d, b'\0'*4096)

    def test_2_readwrite(self):
        offset = 8192
        lsvd.cache_write(offset, b'A'*512)
        lsvd.cache_write(offset+1024, b'B'*512)
        d = lsvd.cache_read(offset, 2048)
        self.assertEqual(d, b'A'*512+b'\0'*512+b'B'*512+b'\0'*512)

if __name__ == '__main__':
    startup()
    unittest.main(exit=False)
    lsvd.cache_shutdown()
    lsvd.shutdown()
    time.sleep(1)

