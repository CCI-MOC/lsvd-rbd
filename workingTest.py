#!/usr/bin/python3

#We shall see what happens

from ctypes import *
from lsvd_types import *
import matplotlib
import matplotlib.pyplot as plt
import unittest
import lsvd
import os
import ctypes
import time
import mkcache
import mkdisk
import random
import string

dirb = os.getcwd()
lsvd_lib = CDLL(dirb + "/liblsvd.so")
assert lsvd_lib

nvme = '/tmp/nvme'
img = '/tmp/bkt/obj'
dir = os.path.dirname(img)

import signal 				#present in all files. Will experiment what it does
signal.signal(signal.SIGINT, signal.SIG_DFL)

xlate,wcache,rcache = None,None,None	#init for variables

def startup():
	global xlate,wcache,rcache 	#xlate needed for cache start ups
	mkdisk.cleanup(img)		#checks disk path
	sectors = 10*2*1024		#10MB
	mkdisk.mkdisk(img, sectors)
	mkcache.mkcache(nvme)
	xlate = lsvd.translate(img, 1, True)
	wcache = lsvd.write_cache(nvme)
	wcache.init(xlate,1)
	rcache = lsvd.read_cache(xlate, 2, wcache._fd)
	time.sleep(0.1)

def finish():
	wcache.shutdown()
	rcache.close()
	xlate.close()

def avge(arr, start, end):
	return sum(arr[start:end]) / (end - start)

class tests(unittest.TestCase):
	def setUp(self):
		self.startTime = time.time()

	def tearDown(self):
		t = time.time() - self.startTime
		print('%s: %.3f' % (self.id(), t))

	def test_1_zeroes_read(self): 	#Writing a case without reference using only available functions
		startup()
		for x in range(10): 	#Tests both different offsets and multiple sizes
			data = rcache.read(x*512, (x+1)*512)
			self.assertEqual(data, b'\0'*(x+1)*512)
		finish()


	def test_2_read_after_writecache_reset(self):
		startup()
		offset = 1024
		reps = 50
		arr = [0]*reps
		for x in range(reps):
			st = time.time()
			wcache.write(offset*4*x, b'A'*1024)
			wcache.write(offset*4*x+2048, b'B'*1024)
			d = wcache.read(offset*4*x, 4096)
			self.assertEqual(d, b'A'*1024+b'\0'*1024+b'B'*1024+b'\0'*1024)
			wcache.w_cache_close()
			#wcache = lsvd.write_cache(nvme)
			wcache.init(xlate,1)
			ft = time.time() - st
			#print('%s %d: %.3f' % (self.id() + '.test ', x, ft))
			arr[x] = ft
		botavg = sum(arr[0:9]) / len(arr[0:9])
		topavg = sum(arr[reps-10:reps-1]) / len(arr[reps-10:reps-1])
		print('%s: %.3f  |  %s: %.3f' % ('botavg', botavg, 'topavg', topavg))
		time.sleep(0.1)
		for y in range(reps):
			d = xlate.read(offset*4*y, 4096)
			self.assertEqual(d, b'A'*1024+b'\0'*1024+b'B'*1024+b'\0'*1024)
		wcache.wcache_reset()
		d = wcache.read(offset, 2048)
		self.assertEqual(d, b'\0'*512+b'\0'*512+b'\0'*512+b'\0'*512)
		finish()

	def test_3_read_after_img_reset(self):
		global wcache
		startup()
		time.sleep(0.01)
		offset = 4096*3
		reps = 50
		arr = [0]*reps
		for x in range(reps):
			st = time.time()
			wcache.write(offset*x, b'W'*4096)
			wcache.write(offset*x + 4096, b'X'*4096)
			wcache.write(offset*x + 8192, b'Y'*4096)
			time.sleep(0.1)

			m1 = wcache.getmap(0, 1000)
			wcache.checkpoint()
			m2 = wcache.getmap(0, 1000)
			self.assertEqual(m1, m2)

			wcache.shutdown()
			wcache = lsvd.write_cache(nvme)
			wcache.init(xlate,1)

			m3 = wcache.getmap(0, 1000)
			self.assertEqual(m1, m3)
			d = xlate.read(offset*x, 4096*3)
			self.assertEqual(d, b'W'*4096 + b'X'*4096 + b'Y'*4096)
			ft = time.time() - st
			#print('%s %d: %.3f' % (self.id() + '.test ', x, ft))
			arr[x] = ft
		time.sleep(0.01)
		botavg = sum(arr[0:9]) / len(arr[0:9])
		topavg = sum(arr[reps-10:reps-1]) / len(arr[reps-10:reps-1])
		print('%s: %.3f  |  %s: %.3f' % ('botavg', botavg, 'topavg', topavg))

		finish()

	def test_4_randwrite_overflow(self):
		startup()
		reps = 200
		arr = [0]*reps
		numWrites = 100
		t = list(range(reps))
		for x in range(reps):
			st = time.time()
			for y in range(numWrites):
				L = random.choice(string.ascii_letters)
				d = L.encode('UTF-8')
				wcache.write(512*random.randint(0,200),d*512) 
			ft = time.time() - st
			arr[x] = ft
		botavg = sum(arr[0:9]) / len(arr[0:9])
		topavg = sum(arr[reps-10:reps-1]) / len(arr[reps-10:reps-1])
		print('%s: %.3f  |  %s: %.3f' % ('botavg', botavg, 'topavg', topavg))
		matplotlib.use('Agg')
		plt.plot(t,arr)
		plt.xlabel('1000 writes per tick')
		plt.ylabel('time in seconds')
		plt.title('Time it takes per 1000 writes')
		plt.savefig('Time_per_writes.png')
		finish()

if __name__ == '__main__':
	lsvd.io_start()
	suite = unittest.TestLoader().loadTestsFromTestCase(tests)
	unittest.TextTestRunner(verbosity=0).run(suite)
	#unittest.main(exit=False)
	lsvd.io_stop()
	time.sleep(0.1)


