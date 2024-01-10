#!/usr/bin/python3

import unittest
import lsvd
import os
import ctypes
import time
import mkcache
import mkdisk

import signal
signal.signal(signal.SIGINT, signal.SIG_DFL)

import pickle
with open('test6-oplist1.pickle', 'r') as f:
    oplist = pickle.load(f)

with open('test6-oplist5.pickle', 'r') as f:
    oplist_5 = pickle.load(f)

os.environ["LSVD_BACKEND"] = "file"
os.environ["LSVD_CONFIG_FILE"] = "/dev/null"
if 'DEBUG_CACHE' in os.environ:
    nvme = os.environ['DEBUG_CACHE']
    os.environ['LSVD_CACHE_DIR'] = os.path.dirname(nvme)
else:
    nvme = '/tmp/obj.cache'    
if 'DEBUG_OBJ' in os.environ:
    img = os.environ['DEBUG_OBJ']
else:
    img = '/tmp/bkt/obj'

print(nvme, img)
dir = os.path.dirname(img)

def div_round_up(n, m):
    return (n + m - 1) // m

def mk_300m_cache(nvme):
    mkcache.cleanup(nvme)
    pages = 300*1024*1024 // 4096
    r_units = int(0.75 * pages) // 16
    r_pages = r_units * 16
    r_oh = div_round_up(r_units * lsvd.sizeof_obj_offset, 4096)
    w_pages = pages - r_pages - r_oh - 3
    mkcache.mkcache(nvme, wblks=w_pages, rblks=r_pages)

def rbd_startup():
    mkdisk.cleanup_files(img)
    sectors = 10*1024*1024*2 # 10GB
    mkdisk.mkdisk(img, sectors)
    mk_300m_cache(nvme)
    return lsvd.rbd_open(img)

def rbd_finish(_img):
    lsvd.rbd_close(_img)

# "forward reference"
def get_oplist():
    return oplist

def get_ui_oplist():
    return oplist_5

import random
random.seed(17)
import zlib
import time

def reset_checksums():
    global checksums
    checksums = dict()

if 'DEBUG_NORANDOM' in os.environ:
    _rnd_data = b'x' * (40000*512)
else:
    _rnd_data = bytes([random.randint(0,255) for _ in range(40000*512)])

def random_data(nbytes):
    offset = random.randint(0,7000*512)
    return _rnd_data[offset:offset+nbytes]

def set_cksum(sector, data):
    global checksums
    for i in range(0, len(data), 512):
        buf = data[i:i+512]
        checksums[sector] = zlib.crc32(buf)
        #print("cksum[%d] = %08x" % (sector, checksums[sector]))
        sector += 1
        
def check_cksum(sector, data):
    global checksums
    for i in range(0, len(data), 512):
        buf = data[i:i+512]
        if sector in checksums:
            x = zlib.crc32(buf)
            #print("cksum[%d] = %08x? %08x" % (sector, checksums[sector], x))
            if x != checksums[sector]:
                print("fail: cksum[%d] %x%s != %x" %
                          (sector, x, " (zeros)" if x==0xb2aa7578 else "",
                               checksums[sector]))
                return False
        else:
            #print("cksum[%d] blank" % sector)
            if buf != b'\0'*512:
                return False
        sector += 1
    return True
    
# every sector is (deterministic) random data, with LBA in first 4 bytes
def stamp_data(sector, data):
    d = bytearray(data)
    for i in range(0, len(data), 512):
        d[i:i+4] = sector.to_bytes(4, 'little')
        sector += 1
    return bytes(d)

class tests(unittest.TestCase):

    def test_0_null(self):
        pass
    
    def test_1_qemu_ops(self):
        print('test_1_qemu_ops')
        reset_checksums()
        _img = rbd_startup()
        for op,sector,n_sectors in get_oplist():
            print(op, sector, n_sectors)
            if op == 'w':
                data = random_data(n_sectors*512)
                set_cksum(sector, data)
                lsvd.rbd_write(_img, sector*512, data)
            elif op == 'r':
                data = lsvd.rbd_read(_img, sector*512, n_sectors*512)
                self.assertTrue(check_cksum(sector, data))
            elif op == 'f':
                lsvd.rbd_flush(_img)
            else:
                self.assertTrue(False)

        print('waiting...')
        time.sleep(2)
        sectors = 10*1024*1024*2 # 10GB
        print('checking image:')
        for i in range(0, sectors, 128):
            data = lsvd.rbd_read(_img, i*512, 128*512)
            for j in range(128):
                s = i + j
                _msg = "sector %d" % s
                _data = data[j*512:(j+1)*512]
                if s in checksums:
                    crc = zlib.crc32(_data)
                    self.assertEqual(crc, checksums[s], msg=_msg)
                else:
                    self.assertEqual(_data, b'\0'*512, msg=_msg)

        lsvd.rbd_close(_img)

        print('checking image after close/open:')
        _img2 = lsvd.rbd_open(img)
        print('waiting...')
        time.sleep(2)
        
        for i in range(0, sectors, 128):
            data = lsvd.rbd_read(_img2, i*512, 128*512)
            for j in range(128):
                s = i + j
                _msg = "sector %d" % s
                _data = data[j*512:(j+1)*512]
                if s in checksums:
                    crc = zlib.crc32(_data)
                    self.assertEqual(crc, checksums[s], msg=_msg)
                else:
                    self.assertEqual(_data, b'\0'*512, msg=_msg)

    # mimics I/O order, iovec structure, and completion order of sample ubuntu14 install
    def test_2_ubuntu_install(self):
        reset_checksums()
        _img = rbd_startup()
        w_handles = dict()
        r_handles = dict()
        i = 0
        for t in get_ui_oplist():
            i += 1
            if i == 1000:
                print('+',end='',flush=True);
                i = 0
            if t[0] == 'wx':
                addr = t[1]
                io,_sector,data = w_handles[addr]
                assert(_sector == addr)
                lsvd.rbd_wait_writev(io)
                set_cksum(_sector, data)  # comment out to just use C asserts
                del w_handles[addr]
            elif t[0] == 'rx':
                addr = t[1]
                io,_sector = r_handles[addr]
                assert(_sector == addr)
                data = lsvd.rbd_wait_readv(io)
                check_cksum(_sector,data) # comment out to just use C asserts
                del r_handles[addr]
            elif t[0] == 'w':
                op,sector,n_sectors,sizes = t
                data = random_data(n_sectors*512)
                data = stamp_data(sector, data)
                io = lsvd.rbd_launch_writev(_img, sector*512, data, sizes)
                w_handles[sector] = [io,sector,data]
            elif t[0] == 'r':
                op,sector,n_sectors,sizes = t
                io = lsvd.rbd_launch_readv(_img, sector*512, sizes)
                r_handles[sector] = [io, sector]

        print('checking image:')
        print('waiting...')
        time.sleep(2)
        sectors = 10*1024*1024*2 # 10GB
        for i in range(0, sectors, 128):
            data = lsvd.rbd_read(_img, i*512, 128*512)
            for j in range(128):
                s = i + j
                _msg = "sector %d" % s
                _data = data[j*512:(j+1)*512]
                if s in checksums:
                    crc = zlib.crc32(_data)
                    self.assertEqual(crc, checksums[s], msg=_msg)
                else:
                    self.assertEqual(_data, b'\0'*512, msg=_msg)


        print('checking image after close/open:')
        lsvd.rbd_close(_img)
        _img2 = lsvd.rbd_open(img)
        for i in range(0, sectors, 128):
            data = lsvd.rbd_read(_img2, i*512, 128*512)
            for j in range(128):
                s = i + j
                _msg = "sector %d" % s
                _data = data[j*512:(j+1)*512]
                if s in checksums:
                    crc = zlib.crc32(_data)
                    self.assertEqual(crc, checksums[s], msg=_msg)
                else:
                    self.assertEqual(_data, b'\0'*512, msg=_msg)

if __name__ == '__main__':
    unittest.main(exit=False)
    time.sleep(0.1)

