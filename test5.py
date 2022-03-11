#!/usr/bin/python3

import unittest
import lsvd
import os
import ctypes
import time
import mkcache
import mkdisk

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
    lsvd.fake_rbd_init()
    time.sleep(0.1) # let evict_thread start up in valgrind

def finish():
    lsvd.rcache_shutdown()
    lsvd.wcache_shutdown()
    lsvd.cache_close()
    lsvd.shutdown()

class tests(unittest.TestCase):

    def test_1_wcache_holes(self):
        startup()
        lsvd.write(0, b'A'*20*1024)
        lsvd.wcache_write(4096, b'B'*4096)
        lsvd.wcache_write(3*4096, b'C'*4096)
        d = lsvd.fake_rbd_read(0, 20*1024)

        self.assertEqual(d, b'A'*4096 + b'B'*4096 + b'A'*4096 + b'C'*4096 + b'A'*4096)

if __name__ == '__main__':
    unittest.main(exit=False)
    time.sleep(0.1)

