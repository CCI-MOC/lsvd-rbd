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
    lsvd.init(img, 1, False)
    mkcache.mkcache(nvme)

    lsvd.cache_open(nvme)
    lsvd.wcache_init(1)
    lsvd.rcache_init(2)

def finish():
    lsvd.rcache_shutdown()
    lsvd.wcache_shutdown()
    lsvd.shutdown()
    
class tests(unittest.TestCase):

    def test_1_read_zeros(self):
        d = lsvd.rcache_read(0, 4096)
        self.assertEqual(d, b'\0'*4096)
        d = lsvd.rcache_read(13*512, 3*512)
        self.assertEqual(d, b'\0'*(3*512))

if __name__ == '__main__':
    startup()
    unittest.main(exit=False)
    finish()
    time.sleep(0.1)

