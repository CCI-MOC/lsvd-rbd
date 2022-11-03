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

os.environ["LSVD_BACKEND"] = "file"
os.environ["LSVD_CONFIG_FILE"] = "/dev/null"
nvme = '/tmp/nvme'
img = '/tmp/bkt/obj'
dir = os.path.dirname(img)

import signal
signal.signal(signal.SIGINT, signal.SIG_DFL)

xlate,wcache,rcache = None,None,None

def startup():
    global xlate,wcache,rcache
    mkdisk.cleanup_files(img)
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

    # for this and following tests, note that the default test
    # read cache (from mkcache.py) is 4K *4KB = 256 blocks
    
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
        self.assertEqual(m, [([1,0],255)])

        d = rcache.read(0, 4096)
        self.assertEqual(d, expected)

        xlate.fakemap_update(8*32, 600, 1, 33 + 8*32)
        # this reads the first 33 pages = 132K
        time.sleep(0.01)
        d = rcache.read(16*4096, 17*4096)

        time.sleep(0.1)
        m = rcache.getmap()
        self.assertEqual(m, [([1, 0], 255), ([1, 1], 254), ([1, 2], 253)])
        finish()

    def test_3_add_fake(self):
        # print('Test 3')
        startup()

        # write 64K of 'A' at LBA=24, obj=2, offset=0
        data = b'A'*65536
        rcache.add(2,0,data)         
        xlate.fakemap_update(24, 128+24, 2, 0)

        d = rcache.read(24*512, 4096)
        self.assertEqual(d, data[0:4096])
        d = rcache.read((128+24-8)*512, 8192)
        self.assertEqual(d, b'A'*4096 + b'\0'*4096)
        self.assertEqual(rcache.flatmap(), [[0,0]] * 255 + [[2, 0]])

        rcache.add(2,128,b'B'*65536)
        rcache.add(2,256,b'C'*65536)
        self.assertEqual(rcache.flatmap(), [[0,0]] * 253 + [[2,256], [2,128],[2, 0]])

        finish()

    # use rcache.read2, with different internal logic
    def test_4_add_fake(self):
        # print('Test 4')
        startup()

        # write 64K of 'A' at LBA=24, obj=2, offset=0
        data = b'A'*65536
        rcache.add(2,0,data)         
        xlate.fakemap_update(24, 128+24, 2, 0)

        d = rcache.read2(24*512, 4096)
        self.assertEqual(d, data[0:4096])
        d = rcache.read2((128+24-8)*512, 8192)
        self.assertEqual(d, b'A'*4096 + b'\0'*4096)
        self.assertEqual(rcache.flatmap(), [[0,0]] * 255 + [[2, 0]])

        rcache.add(2,128,b'B'*65536)
        rcache.add(2,256,b'C'*65536)
        self.assertEqual(rcache.flatmap(), [[0,0]] * 253 + [[2,256], [2,128],[2, 0]])

        finish()
        
    def test_5_evict(self):
        #print('Test 5')
        startup()
        data = b'X'*64*1024
        for i in range(16):
            rcache.add(i, 0, data)
        m = map_tuples(rcache.getmap())
        self.assertEqual(m, [(i,0,255-i) for i in range(16)])
        s1 = set(m)

        rcache.evict(4)
        m = map_tuples(rcache.getmap())
        s2 = set(m)
        self.assertEqual(len(s1-s2), 4)

        for i in range(50,54):
            rcache.add(i, 0, data)
        s3 = set(map_tuples(rcache.getmap()))

        self.assertEqual(len(s3-s2), 4)
        self.assertEqual(len(s3-s1), 4)
        
        finish()
        
if __name__ == '__main__':
    lsvd.io_start()
    unittest.main(exit=False)
    lsvd.io_stop()
    time.sleep(0.1)
