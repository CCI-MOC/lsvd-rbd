// file:        extent.cc
// description: Extent map for S3 Block Device
// author:      Peter Desnoyers, Northeastern University
//              Copyright Peter Desnoyers, 2021
// license:     GNU LGPL v2.1 or newer
//

#include <cstddef>
#include <stdint.h>
#include <stdlib.h>
#include <vector>
#include <list>
#include <tuple>
#include <cassert>

// LBA -> backend:   map<lba_len,obj_offset>
// backend -> cache: map<obj_offset_len,lba>
namespace extmap {

    // map inputs:
    // lba:     for volume map
    // buf_ptr: temporary buffer maps
    struct lba {
	int64_t lba;
    public:
	buf_ptr operator+=(int val) {
	    lba += val;
	    return *this;
	}
	buf_ptr operator+(int val) {
	    return (lba){.lba = lba + val};
	}
	bool operator==(const lba &other) {
	    return lba == other.lba;
	}
    };

    // note that all extents are in units of sectors
    struct buf_ptr {
	char *ptr;
    public:
	buf_ptr operator+=(int val) {
	    ptr += 512 * val;
	    return *this;
	}
	buf_ptr operator+(int val) {
	    return (buf_ptr){.ptr = ptr + 512 * val};
	}
	bool operator==(const buf_ptr &other) {
	    return ptr == other.ptr;
	}
    };

    struct obj_offset {
	int32_t obj;		// TODO: 40-bit obj#, 24-bit offset
	int32_t offset;		// 2^24 sectors = 8GB
    public:
	obj_offset operator+=(int val) {
	    offset += val;
	    return *this;
	}
	obj_offset operator+(int val) {
	    return (obj_offset){.obj = obj, .offset = offset + val};
	}
	bool operator==(const obj_offset &other) {
	    return obj == other.obj && offset == other.offset;
	}
    };


    
    struct lba_len {
	int64_t _lba : 40;
	int64_t _len  : 24;
    public:
	int64_t base(void) { return _lba; }
	int64_t base(int64_t b) {
	    auto limit = _lba + _len;
	    _lba = b;
	    _len = limit - b;
	    return b;
	}
	int64_t limit(void) {return _lba + _len; }
	int64_t limit(int64_t lim) {
	    _len = lim - _lba;
	    return lim;
	}

	// damn, need to figure out what's different between this and
	// operator<(const baselen & other) {
	//
	bool operator<(const baselen other) const {
	    return _base < other._base;
	}
	bool operator<(const int64_t other) const {
	    return _base < other;
	}
    };

    struct obj_offset_len {
	int32_t _obj;
	int32_t _offset;
	int32_t _limit;
    public:
	int64_t base(void) { return _offset; }
	int64_t base(int64_t b) { _offset = b; return b;}
	int64_t limit(void) {return _limit; }
	int64_t limit(int64_t lim) { _limit = lim; return lim;}
	bool operator<(const baselen other) const {
	    return _offset < other._offset;
	}
	bool operator<(const int64_t other) const {
	    return _offset < other;
	}
    };
	
    template <class S, class T>
    struct _extent {
	S addr;
	T ptr;
    public:
	struct _extent operator+=(int val) {
	    ptr += val;
	    return *this;
	}
	bool operator<(const _extent other) const {
	    return addr.base() < other.addr.base();
	}
	std::tuple<int64_t, int64_t,T> vals(void) {
	    return std::make_tuple(addr.base(), addr.limit(), ptr);
	}
    };
}
