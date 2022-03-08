#!/usr/bin/python3

import unittest
import lsvd
import os
import ctypes
import time
import mkcache
import mkdisk
import test2 as t2
import test3 as t3

nvme = '/tmp/nvme'
img = '/tmp/bkt/obj'
dir = os.path.dirname(img)

def startup():
    mkdisk.cleanup(img)
    sectors = 10*1024*2 # 10MB
    mkdisk.mkdisk(img, sectors)
    mkcache.mkcache(nvme)
    lsvd.init(img, 1, True)

    lsvd.cache_open(nvme)
    lsvd.wcache_init(1)
    lsvd.rcache_init(2)
    time.sleep(0.1) # let evict_thread start up in valgrind

def finish():
    lsvd.rcache_shutdown()
    lsvd.wcache_shutdown()
    lsvd.cache_close()
    lsvd.shutdown()

class tests(unittest.TestCase):

    def test_1_read_zeros(self):
        print('Test 1')
        startup()
        d = lsvd.rcache_read(0, 4096)
        self.assertEqual(d, b'\0'*4096)
        d = lsvd.rcache_read(13*512, 3*512)
        self.assertEqual(d, b'\0'*(3*512))
        finish()

    def test_2_read_fake(self):
        print('Test 2')
        startup()
        t2.write_data_1(img + '.00000001', 0, 1)
        lsvd.fakemap_update(0, 8*32, 1, 33)
        m = lsvd.rcache_getmap()
        self.assertEqual(m, [])
        expected = b''.join([_ * 512 for _ in [b'A', b'B', b'C', b'D', b'E', b'F', b'G', b'H']])
        d = lsvd.rcache_read(0, 4096)

        m = lsvd.rcache_getmap()
        self.assertEqual(d, expected)
        self.assertEqual(m, [([1,0],15)])

        d = lsvd.rcache_read(0, 4096)
        self.assertEqual(d, expected)

        lsvd.rcache_bitmap()
        lsvd.fakemap_update(8*32, 600, 1, 33 + 8*32)
        # this reads the first 33 pages = 132K
        d = lsvd.rcache_read(16*4096, 17*4096)
        m = lsvd.rcache_getmap()
        self.assertEqual(m, [([1, 0], 15), ([1, 1], 14), ([1, 2], 13)])
        finish()
        
if __name__ == '__main__':
    unittest.main(exit=False)
    time.sleep(0.1)

