#
# basic unit tests 
#

import unittest
import lsvd
import os
import ctypes
import time

img = '/tmp/bkt/obj'
dir = os.path.dirname(img)

def blank_uuid():
    return (ctypes.c_ubyte*16)()

def write_data_1(name, ckpt, seq):
    data = bytearray()
    chars = [_ for _ in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ']
    for _ in range(79):
        for c in chars:
            data += (bytes(c,'utf-8') * 512)
    data = data[0:1024*1024]
    data_sectors = len(data) // 512
    
    map_entries = [[_, 1] for _ in range(0, 4096, 2)]
    map_len = lsvd.sizeof_data_map * len(map_entries)
    map_buf = bytearray()
    for lba, sectors in map_entries:
        map_buf += lsvd.data_map(lba, sectors)
    hdr_bytes = len(map_buf) + lsvd.sizeof_hdr + lsvd.sizeof_data_hdr
    if ckpt:
        hdr_bytes += 4
    hdr_sectors = (hdr_bytes + 511) // 512

    h = lsvd.hdr(lsvd.LSVD_MAGIC, 1, blank_uuid(), lsvd.LSVD_DATA,
                     seq, hdr_sectors, data_sectors)

    offset = lsvd.sizeof_hdr + lsvd.sizeof_data_hdr
    ckpt_len = 4 if ckpt else 0
    dh = lsvd.data_hdr(0,                 # last data object
                       offset,            # checkpoints offset
                       ckpt_len,          # 0 or 1 checkpoint
                       0, 0,              # objects cleaned
                       offset + ckpt_len, # map offset
                       len(map_buf))

    hdr = bytearray()
    hdr += h
    hdr += dh
    if ckpt:
        hdr += ctypes.c_uint(ckpt)
    hdr += map_buf
    pad = hdr_sectors*512 - len(hdr)
    hdr += b'\0' * pad

    fp = open(name, 'wb')
    fp.write(hdr)
    fp.write(data)
    fp.close()

# if ckpt > 0, add it
def write_super(name, ckpt, next_obj):
    h = lsvd.hdr(lsvd.LSVD_MAGIC, 1, blank_uuid(), lsvd.LSVD_SUPER, 0, 8, 0)
    #                             ver                     sectors: seq hdr data
    size = 10*1024*1024 // 512 # 10MB
    offset = lsvd.sizeof_hdr + lsvd.sizeof_super_hdr
    ckpt_len = 4 if ckpt else 0
    
    sh = lsvd.super_hdr(size,
                        0,                # total sectors
                        0,                # live sectors
                        next_obj,
                        offset,           # ckpts_offset
                        ckpt_len,
                        0, 0,             # clone bases: offset, len
                        0, 0)             # snapshots: offset, len

    ckpt = ctypes.c_uint(ckpt)            # ignored if len=0
    buf = bytearray() + h + sh + ckpt
    buf += (b'\0' * (4096 - len(buf)))

    fp = open(name, 'wb')
    fp.write(buf)
    fp.close()

def start():
    if not os.access(dir, os.F_OK):
        os.mkdir(dir, 0o755)
    for f in os.listdir(dir):
        os.unlink(dir + "/" + f)

def finish():
    lsvd.shutdown()
    time.sleep(0.1)

def read_ckpt(img):
    f = os.open(img, os.O_RDONLY)
    data = os.read(f, 10000000)
    i1 = lsvd.sizeof_hdr
    i2 = i1 + lsvd.sizeof_ckpt_hdr
    hdr = lsvd.hdr.from_buffer(bytearray(data[0:i1]))
    ckpt_hdr = lsvd.ckpt_hdr.from_buffer(bytearray(data[i1:i2]))
    nckpts = ckpt_hdr.ckpts_len // 4
    c = bytearray(data[ckpt_hdr.ckpts_offset:ckpt_hdr.ckpts_offset+ckpt_hdr.ckpts_len])
    ckpts = (ctypes.c_uint*nckpts).from_buffer(c)

    nexts = ckpt_hdr.map_len // lsvd.sizeof_ckpt_mapentry
    e = bytearray(data[ckpt_hdr.map_offset:ckpt_hdr.map_offset+ckpt_hdr.map_len])
    exts = (lsvd.ckpt_mapentry*nexts).from_buffer(e)

    nobjs = ckpt_hdr.objs_len // lsvd.sizeof_ckpt_obj
    o = bytearray(data[ckpt_hdr.objs_offset:ckpt_hdr.objs_offset+ckpt_hdr.objs_len])
    objs = (lsvd.ckpt_obj*nobjs).from_buffer(o)

    return (hdr, ckpt_hdr, ckpts, objs, exts)

class tests(unittest.TestCase):
    #def setUp(self):
    #def tearDown(self):

    def test_1_size(self):
        #print('Test 1')
        start()
        write_super(img, 0, 1)
        _size = lsvd.init(img, 1)
        self.assertEqual(_size, 10*1024*1024)
        finish()
        
    def test_2_recover(self):
        #print('Test 2')
        start()
        write_super(img, 0, 1)
        write_data_1(img + '.00000001', 0, 1)
        _size = lsvd.init(img, 1)
        self.assertEqual(lsvd.batch_seq(), 2)
        d = lsvd.read(0, 512)
        self.assertEqual(d, b'A' * 512)
        d = lsvd.read(512, 512)
        self.assertEqual(d, b'\0' * 512)
        d = lsvd.read(512*2*20, 512)
        self.assertEqual(d, b'U' * 512)
        d = lsvd.read(512*2*20+512, 512)
        self.assertEqual(d, b'\0' * 512)
        finish()

    def test_3_persist(self):
        #print('Test 3')
        start()
        write_super(img, 0, 1)
        _size = lsvd.init(img, 1)
        d = b'X' * 4096
        lsvd.write(0, d)
        lsvd.write(8192,d)
        lsvd.flush()
        time.sleep(0.1)
        self.assertTrue(os.access('/tmp/bkt/obj.00000001', os.R_OK))
        lsvd.shutdown()

        lsvd.init(img, 1)
        d = lsvd.read(0, 4096)
        self.assertEqual(d, b'X' * 4096)
        finish()

    def test_4_checkpoint(self):
        #print('Test 4')
        start()
        os.system('ls -l /tmp/bkt')
        write_super(img, 0, 1)
        lsvd.init(img, 1)
        d = b'X' * 4096
        lsvd.write(0, d)
        lsvd.write(8192,d)
        lsvd.write(4096,d)
        n = lsvd.checkpoint()
        print('checkpoint', n)
        hdr, ckpt_hdr, ckpts, objs, exts = read_ckpt(img + ('.%08x' % n))
        self.assertEqual([_ for _ in ckpts], [2])
        exts = [_ for _ in map(lambda x: [x.lba,x.len,x.obj,x.offset], exts)]
        self.assertEqual(exts, [[0,8,1,0],[8,8,1,16],[16,8,1,8]])
        finish()

    def test_5_flushthread(self):
        #print('Test 5')
        start()
        write_super(img, 0, 1)
        _size = lsvd.init(img, 1)
        self.assertFalse(os.access('/tmp/bkt/obj.00000001', os.R_OK))        
        d = b'Y' * 4096
        lsvd.write(0, d)
        lsvd.write(8192, d)
        self.assertFalse(os.access('/tmp/bkt/obj.00000001', os.R_OK))        
        time.sleep(3)
        self.assertTrue(os.access('/tmp/bkt/obj.00000001', os.R_OK))        
        lsvd.shutdown()
        self.assertTrue(os.access('/tmp/bkt/obj.00000001', os.R_OK))        

        lsvd.init(img, 1)
        d = lsvd.read(0, 4096)
        self.assertEqual(d, b'Y' * 4096)
        finish()

    # object list persisted correctly in checkpoint
    def test_6_objects(self):
        #printf('Test 6')
        start()
        write_super(img, 0, 1)
        _size = lsvd.init(img, 1)
        d = b'Z' * 4096
        lsvd.write(4096, d)
        self.assertFalse(os.access('/tmp/bkt/obj.00000001', os.R_OK))
        lsvd.flush()
        time.sleep(0.1)
        self.assertTrue(os.access('/tmp/bkt/obj.00000001', os.R_OK))
        self.assertFalse(os.access('/tmp/bkt/obj.00000002', os.R_OK))
        n = lsvd.checkpoint()

        self.assertEqual(n, 2)
        self.assertTrue(os.access('/tmp/bkt/obj.00000002', os.R_OK))

        hdr, ckpt_hdr, ckpts, objs, exts = read_ckpt(img + ('.%08x' % n))
        self.assertEqual(len(objs), 1)
        o = objs[0]
        self.assertEqual(o.seq, 1)
        self.assertEqual(o.data_sectors, 8)
        self.assertEqual(o.live_sectors, 8)
        finish()

    def test_7_objects(self):
        #printf('Test 6')
        start()
        write_super(img, 0, 1)
        _size = lsvd.init(img, 1)
        d = b'Z' * 4096
        lsvd.write(4096, d)
        lsvd.flush()
        time.sleep(0.1)
        n = lsvd.checkpoint()
        self.assertEqual(n, 2)
        self.assertTrue(os.access('/tmp/bkt/obj.00000002', os.R_OK))

        d = b'Q' * 4096
        lsvd.write(4096, d)
        lsvd.flush()
        time.sleep(0.1)
        self.assertTrue(os.access('/tmp/bkt/obj.00000003', os.R_OK))
        self.assertFalse(os.access('/tmp/bkt/obj.00000004', os.R_OK))
        n = lsvd.checkpoint()
        self.assertEqual(n, 4)
        self.assertTrue(os.access('/tmp/bkt/obj.00000004', os.R_OK))

        hdr, ckpt_hdr, ckpts, objs, exts = read_ckpt(img + ('.%08x' % n))
        self.assertEqual(len(objs), 2)
        self.assertEqual([1, 3], [_.seq for _ in objs])
        self.assertEqual(objs[0].data_sectors, 8)
        self.assertEqual(objs[0].live_sectors, 0)
        self.assertEqual(objs[1].data_sectors, 8)
        self.assertEqual(objs[1].live_sectors, 8)

        finish()

from time import sleep
if __name__ == '__main__':
    unittest.main(exit=False)
    sleep(1)
    
