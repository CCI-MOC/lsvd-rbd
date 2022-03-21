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

_vals = None

def inv(i, k, m):
    global _vals
    if not _vals:
        _vals = [None] * m
        for i in range(m):
            _vals[(i*k)%m] = i
    return _vals[i]

def rbd_startup():
    mkdisk.cleanup(img)
    sectors = 10*1024*2 # 10MB
    mkdisk.mkdisk(img, sectors)
    mkcache.mkcache(nvme)
    name = nvme + ',' + img
    return lsvd.rbd_open(name)

def rbd_finish(_img):
    lsvd.rbd_close(_img)

class tests(unittest.TestCase):

    def test_1_wcache_holes(self):
        startup()
        lsvd.write(0, b'A'*20*1024)
        lsvd.flush()
        lsvd.wcache_write(4096, b'B'*4096)
        lsvd.wcache_write(3*4096, b'C'*4096)
        d = lsvd.fake_rbd_read(0, 20*1024)

        self.assertEqual(d, b'A'*4096 + b'B'*4096 + b'A'*4096 + b'C'*4096 + b'A'*4096)
        finish()
        
    def test_2_write(self):
        _img = rbd_startup()
        print('_img', _img)
        data = b'A' * 4096
        for i in range(26):
            lsvd.rbd_write(_img, i*4096, data)
        time.sleep(0.1)
        rbd_finish(_img)

    def test_3_write_read(self):
        _img = rbd_startup()
        c = ord('A')
        for i in range(26):
            data = bytes(chr(c + i), 'utf-8') * 4096
            lsvd.rbd_write(_img, i*4096, data)
        time.sleep(0.1)

        for i in range(26):
            data = bytes(chr(c + i), 'utf-8') * 4096
            d2 = lsvd.rbd_read(_img, i*4096, 4096)
            self.assertEqual(d2, data)
        
        rbd_finish(_img)

    # write cache is 125 pages = 1000 sectors
    # read cache is 16 * 64k blocks = 2048 sectors
    # volume is 10MiB = 20480 sectors = 2650 4KB pages

    def test_4_rand_write(self):
        #s = [bytes(chr(65+i),'utf-8') for i in range(26)]
        #pgs = [_*4096 for _ in s]
        _img = rbd_startup()
        # 59 is enough to force write cache eviction
        N = 200
        pg = 0
        for i in range(N):
            pg = (i * 97) % 2650
            data = bytes('%05d' % pg, 'utf-8') + b'A'*4091
            lsvd.rbd_write(_img, pg*4096, data)

        lsvd.rbd_flush(_img)
        time.sleep(0.2)

        pg = 0
        passed = True
        for i in range(2650):
            j = inv(i, 97, 2650)
            if j >= N:
                continue
            d0 = bytes('%05d' % i, 'utf-8') + b'A'*4091
            d = lsvd.rbd_read(_img, i*4096, 4096)
            if d0 != d:
                passed = False
                print('FAILED:', i, d[0:10]+b'...'+d[-10:], '!=', d0[0:10]+b'...'+d0[-10:])
        time.sleep(4)
        self.assertTrue(passed)

        rbd_finish(_img)

        
if __name__ == '__main__':
    unittest.main(exit=False)
    time.sleep(0.1)

