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

xlate,wcache,rcache = None,None,None

def startup():
    global xlate,wcache,rcache
    mkdisk.cleanup(img)
    sectors = 10*1024*2 # 10MB
    mkdisk.mkdisk(img, sectors)
    mkcache.mkcache(nvme)
    xlate = lsvd.translate(img, 1, True)
    wcache = lsvd.write_cache(nvme)
    wcache.init(xlate,1)
    rcache = lsvd.read_cache(xlate, 2, wcache._fd)
    time.sleep(0.1) # let evict_thread start up in valgrind

def finish():
    wcache.shutdown()
    rcache.close()
    xlate.close()

def map_tuples(L):
    return [(i[0][0],i[0][1],i[1]) for i in L]

class tests(unittest.TestCase):

    def test_1_read_zeros(self):
        #print('Test 1')
        startup()
        d = rcache.read(0, 4096)
        self.assertEqual(d, b'\0'*4096)
        d = rcache.read(13*512, 3*512)
        self.assertEqual(d, b'\0'*(3*512))
        finish()

    def test_2_read_fake(self):
        #print('Test 2')
        startup()
        t2.write_data_1(img + '.00000001', 0, 1)
        xlate.fakemap_update(0, 8*32, 1, 33)
        m = rcache.getmap()
        self.assertEqual(m, [])
        expected = b''.join([_ * 512 for _ in [b'A', b'B', b'C', b'D', b'E', b'F', b'G', b'H']])
        d = rcache.read(0, 4096)

        time.sleep(0.1)                   # wait for async add()
        m = rcache.getmap()
        self.assertEqual(d, expected)
        self.assertEqual(m, [([1,0],15)])

        d = rcache.read(0, 4096)
        self.assertEqual(d, expected)

        rcache.bitmap()
        xlate.fakemap_update(8*32, 600, 1, 33 + 8*32)
        # this reads the first 33 pages = 132K
        d = rcache.read(16*4096, 17*4096)

        time.sleep(0.1)
        m = rcache.getmap()
        self.assertEqual(m, [([1, 0], 15), ([1, 1], 14), ([1, 2], 13)])
        finish()

    def test_3_add_fake(self):
        #print('Test 3')
        startup()

        # write 4K of 'A' at LBA=24, obj=2, offset=0
        data = b'A'*4096
        rcache.add(2,0,data)         
        xlate.fakemap_update(24, 32, 2, 0)

        d = rcache.read(24*512, 4096)
        self.assertEqual(d, data)
        d = rcache.read(24*512, 8192)
        self.assertEqual(d, b'A'*4096 + b'\0'*4096)
        mask = lsvd.rcache_bitmap()
        self.assertEqual(mask[15], 0x0001)
        self.assertEqual(lsvd.rcache_flatmap(), [[0,0]] * 15 + [[2, 0]])

        rcache.add(2,16,b'B'*4096)
        lsvd.rcache_add(2,24,b'C'*(4096*13))
        mask = lsvd.rcache_bitmap()
        self.assertEqual(mask[15], 0xFFFD)
        self.assertEqual(lsvd.rcache_flatmap(), [[0,0]] * 15 + [[2, 0]])

        finish()

    def test_4_evict(self):
        startup()
        data = b'X'*64*1024
        for i in range(16):
            rcache.add(i, 0, data)
        m = map_tuples(lsvd.rcache_getmap())
        self.assertEqual(m, [(i,0,15-i) for i in range(16)])
        s1 = set(m)

        lsvd.rcache_evict(4)
        m = map_tuples(lsvd.rcache_getmap())
        s2 = set(m)
        self.assertEqual(len(s1-s2), 4)

        lsvd.rcache_bitmap()
        for i in range(50,54):
            rcache.add(i, 0, data)
        s3 = set(map_tuples(rcache.getmap()))

        self.assertEqual(len(s3-s2), 4)
        self.assertEqual(len(s3-s1), 4)
        
        finish()
        
if __name__ == '__main__':
    unittest.main(exit=False)
    time.sleep(0.1)

