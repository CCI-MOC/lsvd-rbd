#!/usr/bin/python3

#
# basic unit tests 
#

import unittest
import lsvd
import os

img = "/tmp/bkt/obj"

val = os.system("./mkdisk --size 10m %s" % img)
assert val == 0

_size = lsvd.init(img, 1)
assert _size == 10*1024*1024

class tests(unittest.TestCase):
    def assertZero(self, data, _msg):
        self.assertTrue(data == (b'\0' * len(data)), msg=_msg)

    def test_1_size(self):
        self.assertTrue(_size == 10*1024*1024)
        
    def test_2_readzero(self):
        base = 0
        for L in (512, 4096, 16384):
            for n in (1, 10, 17, 49):
                tmp = lsvd.read(base, L)
                self.assertZero(tmp, "zero: offset %d len %d" % (base, L))
                base += (n*512)

    def test_3_mapsize(self):
        self.assertTrue(lsvd.size() == 0)
        d = 'A' * 512
        lsvd.write(0, d)
        self.assertTrue(lsvd.size() == 1)
        lsvd.write(10*512, d)
        self.assertTrue(lsvd.size() == 2)
        lsvd.write(0, 'B' * (11*512))
        self.assertTrue(lsvd.size() == 1)
        
        
from time import sleep
if __name__ == '__main__':
    unittest.main(exit=False)
    sleep(1)
    lsvd.shutdown()
    
