#!/usr/bin/python3

#
# basic unit tests 
#

import unittest
import lsvd
import os
import mkdisk

import signal
signal.signal(signal.SIGINT, signal.SIG_DFL)

img = "/tmp/bkt/obj"
dir = os.path.dirname(img)
if not os.access(dir, os.F_OK):
    os.mkdir(dir)
for f in os.listdir(dir):
    os.unlink(dir + "/" + f)

sectors = 10*1024*2 # 10MB
mkdisk.mkdisk(img, sectors)

os.environ["LSVD_BACKEND"] = "file"

xlate = lsvd.translate(img, 1, False)
assert xlate.nbytes == 10*1024*1024
_size = xlate.nbytes

#lsvd.io_start()

class tests(unittest.TestCase):
    def assertZero(self, data, _msg):
        self.assertTrue(data == (b'\0' * len(data)), msg=_msg)

    def test_1_size(self):
        self.assertTrue(_size == 10*1024*1024)
        
    def test_2_readzero(self):
        base = 0
        for L in (512, 4096, 16384):
            for n in (1, 10, 17, 49):
                tmp = xlate.read(base, L)
                self.assertZero(tmp, "zero: offset %d len %d" % (base, L))
                base += (n*512)

    def test_3_mapsize(self):
        #print('Test 3')
        self.assertTrue(xlate.mapsize() == 0)
        d = 'A' * 512

        xlate.write(0, d)
        xlate.flush()
        self.assertTrue(xlate.mapsize() == 1)

        xlate.write(10*512, d)
        xlate.flush()
        self.assertTrue(xlate.mapsize() == 2)

        xlate.write(0, 'B' * (11*512))
        xlate.flush()
        self.assertTrue(xlate.mapsize() == 1)
        
    def test_4_readwrite(self):
        #print('Test 4')
        d = b'X' * 4096
        xlate.write(0, d)
        xlate.flush()
        d = xlate.read(1024, 1024)
        self.assertEqual(d, b'X'*1024)
        
    def test_5_readwrite(self):
        #print('Test 5')
        xlate.write(8*1024, b'A'*8192)
        xlate.write(12*1024, b'B'*4096)
        xlate.flush()
        d = xlate.read(8192, 8192)
        self.assertEqual(d, b'A'*4096 + b'B'*4096)
        
    def test_6_readwrite(self):
        #print('Test 6')
        xlate.write(16*1024, b'C'*4096)
        xlate.write(16*1024+512, b'D'*1024)
        xlate.flush()
        d = xlate.read(16*1024, 4096)
        self.assertEqual(d, b'C'*512 + b'D'*1024 + b'C'*2560)
        
from time import sleep
if __name__ == '__main__':
    unittest.main(exit=False)
    sleep(1)
    xlate.close()
#    lsvd.io_stop()
