#!/usr/bin/python3
#
# Python version of LSVD extent map
#  - only handles mapping to object+offset
#  - doesn't merge at insert time, must call .merge() explicitly
#

from bisect import *
import functools

@functools.total_ordering
class extent:
    def __init__(self,base,limit,obj,offset):
        self.base,self.limit,self.obj,self.offset = base,limit,obj,offset

    # functools.total_ordering adds the rest of the methods so that
    # bisect_left works
    #
    def __eq__(self, other):
        return self.limit == other.limit
    def __lt__(self, other):
        return self.limit < other.limit

    # duplicate of vals() overloaded method in extent.h
    #
    def vals(self, base=-1, limit=-1):
        if base == -1:
            return self.base, self.limit, self.obj, self.offset
        elif base >= self.limit:
            return base, base, 0, 0
        elif limit <= self.base:
            return limit, limit, 0, 0
        else:
            _base = max(base, self.base)
            _limit = min(limit, self.limit)
            _offset = self.offset
            if self.base < _base:
                _offset += (_base - self.base)
            return _base, _limit, self.obj, _offset
        
class map:
    def __init__(self):
        # limit, base, obj, offset
        self.exts = []

    def __iter__(self):
        self._i = 0
        return self

    def __next__(self):
        if self._i == len(self.exts):
            raise StopIteration
        tmp = self.exts[self._i]
        self._i += 1
        return tmp

    # returns iterator pointing to:
    #  - extent containing 'base'; if none,
    #  - extent immediately after 'base'; if none,
    #  - end of map
    #
    def lookup(self, base):
        key = extent(base, base, 0, 0)
        i = bisect_left(self.exts, key)
        if i < len(self.exts) and base == self.exts[i].limit:
            i += 1
        self._i = i
        return self

    def size(self):
        return len(self.exts)
    
    # map doesn't do merging at insert time
    #
    def merge(self):
        i = 0
        while i+1 < len(self.exts):
            _b1,_l1,_o1,_f1 = self.exts[i].vals()
            _b2,_l2,_o2,_f2 = self.exts[i+1].vals()
            if _l1 == _b2 and _o1 == _o2 and _f2 == _f1 + (_l2-_b2):
                self.exts[i].limit = _l2
                self.exts.pop(i+1)
            else:
                i += 1

    # to trim, set obj=-1
    #
    def update(self, base, limit, obj, offset):
        key = extent(base, base, 0, 0)
        val = extent(base, limit, obj, offset)
        i = bisect_left(self.exts, key)
        if i < len(self.exts) and base == self.exts[i].limit:
            i += 1
        if i == len(self.exts):
            self.exts.append(val)
            return

        # we bisect an extent
        #   [-----------------]       *it          _new
        #          [+++++]       -> [-----][+++++][----]
        #
        it = self.exts[i]
        if it.base < base and it.limit > limit:
            top = extent(limit, it.limit, it.obj,
                             it.offset + (limit - it.base))
            bottom = extent(it.base, base, it.obj, it.offset)
            self.exts[i] = top
            self.exts.insert(i, bottom)
            i += 1

        # left-hand overlap
        #   [---------]
        #       [++++++++++]  ->  [----][++++++++++]
        #
        elif it.base < base and it.limit > base:
            it.limit = base
            i += 1

        # erase any extents fully overlapped
        #       [----] [---]
        #   [+++++++++++++++++] -> [+++++++++++++++++]
        #
        while i < len(self.exts):
            it = self.exts[i]
            if it.base >= base and it.limit <= limit:
                self.exts.pop(i)
            else:
                break

        # update right-hand overlap
        #        [---------]
        #   [++++++++++]        -> [++++++++++][----]
        #
        if i < len(self.exts):
            it = self.exts[i]
            if limit > it.base:
                it.offset += (limit - it.base)
                it.base = limit

        if obj != -1:
            self.exts.insert(i, val)


if __name__ == '__main__':
    # do lots of random updates, saving in both extent map and
    # a flat obj/offset map
    import random
    a = map()
    N_LBAS = 1000
    #random.seed(17)
    objs = N_LBAS * [-1]
    offsets = N_LBAS * [-1]

    def verify():
        b,l = -1,-1
        for e in a:
            base,limit,_,_ = e.vals()
            assert(base <= limit)
            assert(base >= l)
            b,l = base,limit

    def test():
        n = 1000 * [False]
        for e in a:
            base,limit,obj,offset = e.vals()
            for i in range(limit-base):
                assert(not n[base])
                n[base] = True
                assert(objs[base] == obj)
                assert(offsets[base] == offset)
                base += 1
                offset += 1
        for i in range(1000):
            assert(n[i] or objs[i] == -1)
    
    for x in range(10000):
        base = random.randint(0,999)
        _len = random.randint(0, min(20,999-base))
        obj = random.randint(0,19)
        offset = random.randint(0,99)
        a.update(base, base+_len, obj, offset)
        for i in range(_len):
            objs[base] = obj
            offsets[base] = offset
            base += 1
            offset += 1
        verify()
        test()
