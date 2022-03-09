#!/usr/bin/python3

import unittest
import lsvd
import os
import ctypes
import time
import mkcache
import test2 as t2

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
    lsvd.cache_open(nvme)
    lsvd.wcache_init(1)
    fd2 = os.open(nvme, os.O_RDONLY)

def c_super(fd):
    print('fd', fd)
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

def restart():
    lsvd.wcache_shutdown()
    lsvd.shutdown()
    for f in os.listdir(dir):
        os.unlink(dir + "/" + f)
    t2.write_super(img, 0, 1)
    mkcache.mkcache(nvme)
    lsvd.init(img, 1, True)
    lsvd.wcache_init(1)
    
class tests(unittest.TestCase):
    #def setUp(self):
    #def tearDown(self):

    def test_1_readwrite(self):
        lsvd.wcache_write(0, b'X'*4096)
        m = lsvd.wcache_getmap(0, 1000)
        # header is block 3, data starts on 4
        self.assertEqual(m, [[0,8,(4*8)]])
        d = lsvd.wcache_read(0, 4096)
        self.assertEqual(d, b'X'*4096)
        d = lsvd.wcache_read(4096, 4096)
        self.assertEqual(d, b'\0'*4096)

    def test_2_readwrite(self):
        offset = 8192
        lsvd.wcache_write(offset, b'A'*512)
        lsvd.wcache_write(offset+1024, b'B'*512)
        d = lsvd.wcache_read(offset, 2048)
        self.assertEqual(d, b'A'*512+b'\0'*512+b'B'*512+b'\0'*512)

    def test_3_extents(self):
        lsvd.wcache_shutdown()
        mkcache.mkcache(nvme)
        lsvd.wcache_init(1)
        
        wsup = c_w_super(fd2, 1)
        n = wsup.next
        lsvd.wcache_write(0, b'X'*8192)
        m = lsvd.wcache_getmap(0, 1000)

        h,e = c_hdr(fd2, n)
        ee = [[_.lba, _.len] for _ in e]
        self.assertEqual(h.seq, 1)
        self.assertEqual(ee, [[0,16]])
        
        lsvd.wcache_write(1024, b'A'*512)
        n += h.len
        h,e = c_hdr(fd2, n)
        ee = [[_.lba, _.len] for _ in e]
        self.assertEqual(h.seq, 2)
        self.assertEqual(ee, [[2,1]])

        lsvd.wcache_write(2048, b'B'*512)
        n += h.len
        h,e = c_hdr(fd2, n)
        ee = [[_.lba, _.len] for _ in e]
        self.assertEqual(h.seq, 3)
        self.assertEqual(ee, [[4,1]])

        n,d,e = lsvd.wcache_oldest(3)
        print('m', m, 'd', d, 'e', e)

    def test_4_backend(self):
        restart()

        d = lsvd.read(0, 4096*3)
        self.assertEqual(d, b'\0'*3*4096)

        lsvd.wcache_write(0, b'W'*4096)
        lsvd.wcache_write(4096, b'X'*4096)
        lsvd.wcache_write(8192, b'Y'*4096)
        time.sleep(0.1)
        d = lsvd.read(0, 4096*3)
        self.assertEqual(d, b'W'*4096 + b'X'*4096 + b'Y'*4096)

    def test_5_cleaning(self):
        restart()
        
        
if __name__ == '__main__':
    startup()
    unittest.main(exit=False)
    lsvd.wcache_shutdown()
    lsvd.shutdown()
    time.sleep(1)

