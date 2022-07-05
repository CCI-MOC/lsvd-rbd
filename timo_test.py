#!/usr/bin/python3

# Timo's first lsvd test

import unittest
import lsvd
import os
import ctypes
import time
import mkdisk
import mkcache

import signal
signal.signal(signal.SIGINT, signal.SIG_DFL)

nvme = '/tmp/nvme'
img = '/tmp/bkt/obj'
dir = os.path.dirname(img)

xlate,wcache,rcache = None,None,None

def rbd_startup():
	global wcache, xlate
	global fd
	mkdisk.cleanup(img)
	sectors = 10*1024*2 #  10MB
	mkdisk.mkdisk(img, sectors)
	mkcache.mkcache(nvme)
	wcache = lsvd.write_cache(nvme)

	name = nvme + ',' + img
	return lsvd.rbd_open(name)

def rbd_finish(_img):
    	lsvd.rbd_close(_img)

class tests(unittest.TestCase):

	def test_test(self):
		_img = rbd_startup()
		time.sleep(0.1)
		rbd_finish(_img)

	def write_read(self):
		_img = rbd_startup()
                time.sleep(0.1)
                rbd_finish(_img)

if __name__ == '__main__':
	lsvd.io_start()
	
	unittest.main(exit=False)
	
	time.sleep(0.1)
	lsvd.io_stop()
