// file:        extent.cc
// description: Extent map for S3 Block Device
// author:      Peter Desnoyers, Northeastern University
//              Copyright Peter Desnoyers, 2021
// license:     GNU LGPL v2.1 or newer
//

// There may be an elegant way to do this in C++; this isn't it.

// Defines three types of map:
//  objmap (int64_t => obj_offset)   - maps LBA to obj+offset
//  cachemap (obj_offset => int64_t) - maps obj+offset to LBA
//  bufmap (int64_t => sector_ptr)   - maps LBA to char*
//
// Update/trim takes a pointer to a vector for discarded extents, so
// we don't have to search the map twice, and we can use read-only
// iterators for lookup

#include <cstddef>
#include <stdint.h>
#include <stdlib.h>
#include <vector>
#include <set>
#include <tuple>
#include <cassert>

namespace extmap {

    struct obj_offset {
	int64_t obj    : 36;
	int64_t offset : 28;	// 128GB
    public:
	obj_offset operator+=(int val) {
	    offset += val;
	    return *this;
	}
	bool operator==(const obj_offset other) const {
	    return (obj == other.obj) && (offset == other.offset);
	}
	bool operator<(const obj_offset other) const {
	    return (obj == other.obj) ? (offset < other.offset) : (obj < other.obj);
	}
	bool operator>(const obj_offset other) const {
	    return (obj == other.obj) ? (offset > other.offset) : (obj > other.obj);
	}
	bool operator>=(const obj_offset other) const {
	    return (obj == other.obj) ? (offset >= other.offset) : (obj >= other.obj);
	}
	bool operator<=(const obj_offset other) const {
	    return (obj == other.obj) ? (offset <= other.offset) : (obj <= other.obj);
	}
	obj_offset operator+(int val) {
	    return (obj_offset){.obj = obj, .offset = offset + val};
	}
	int operator-(const obj_offset other) {
	    if (obj != other.obj)
		return 0;
	    return offset - other.offset;
	}
    };

    struct sector_ptr {
	char *buf;
    public:
	sector_ptr(char *ptr) {
	    buf = ptr;
	}
	sector_ptr() {}
	sector_ptr operator+=(int val) {
	    buf += val*512;
	    return *this;
	}
	bool operator<(const sector_ptr other) const {
	    return buf < other.buf;
	}
	bool operator>(const sector_ptr other) const {
	    return buf > other.buf;
	}
	bool operator==(const sector_ptr other) const {
	    return buf == other.buf;
	}
	sector_ptr operator+(int val) {
	    sector_ptr p(buf + val*512);
	    return p;
	}
	int operator-(const sector_ptr val) {
	    return (buf - val.buf) / 512;
	}
    };
    
    // These are the three map types we support. There's probably a way to 
    // do this with a template, but I don't think it's worth the effort
    //
    struct _lba2buf {
	int64_t    base : 40;
	int64_t    len  : 24;
	sector_ptr ptr;
    };
	
    struct _obj2lba {
	obj_offset base;
	int64_t    len : 24;
	int64_t    ptr : 40;	// LBA
    };

    struct _lba2obj {
	int64_t    base : 40;
	int64_t    len  : 24;
	obj_offset ptr;
    };

    template <class T, class T_in, class T_out> 
    struct _extent {
	T s;
    public:
	_extent(T_in _base, int64_t _len, T_out _ptr) {
	    s.base = _base;
	    s.len = _len;
	    s.ptr = _ptr;
	}
	_extent(T_in _base) {
	    s.base = _base;
	}
	_extent() {}
	T_in base(void) { return s.base; }
	T_in limit(void) { return s.base + s.len; }
	void limit(T_in _limit) { s.len = _limit - s.base; }
	void base(T_in _base) {
	    auto delta = _base - s.base;
	    s.ptr += delta;
	    s.len -= delta;
	    s.base = _base;
	}
	
	_extent operator+=(int val) {
	    s.ptr += val;
	    return *this;
	}
	bool operator<(const _extent &other) const {
	    return s.base < other.s.base;
	}
    };

    // template <class T, class T_in, class T_out> 
    typedef _extent<_lba2buf,int64_t,sector_ptr> lba2buf;
    typedef _extent<_obj2lba,obj_offset,int64_t> obj2lba;
    typedef _extent<_lba2obj,int64_t,obj_offset> lba2obj;

    template <class T, class T_in>
    struct x_pair {
	mutable T_in max;
	std::vector<T> *list;
    public:
	bool operator<(const x_pair &other) const {
	    return max < other.max;
	}
    };

    template <class T, class T_in, class T_out>
    struct extmap {
	static const int _load = 256;

	typedef          x_pair<T,T_in>        local_xpair;
	typedef          std::set<local_xpair> l1_map;
	typedef typename l1_map::iterator      l1_iter;
	typedef          std::vector<T>        l2_map;
	typedef typename l2_map::iterator      l2_iter;
	
	l1_map the_map;
	//std::set<x_pair<T>> the_map;

	// slices off [len]..[end] and returns it
	//
	static std::vector<T> *slice(std::vector<T> *A, int len) {
	    auto half = new std::vector<T>();
	    half->reserve(_load);
	    for (auto it = A->begin()+len; it != A->end(); it++)
		half->push_back(*it);
	    A->resize(len);
	    return half;
	}
	
	bool next_to_last(l1_iter it) {
	    it++;
	    return it == the_map.end();
	}

	l1_iter last(void) {
	    auto it = the_map.end();
	    it--;
	    return it;
	}

	l1_iter next(l1_iter it) {
	    it++;
	    return it;
	}
	l1_iter prev(l1_iter it) {
	    it--;
	    return it;
	}

	class iterator {
	public:
	    extmap *m;
	    l1_iter l1;
	    l2_iter l2;
	    using iterator_category = std::random_access_iterator_tag;
	    using value_type = T;
	    using difference_type = std::ptrdiff_t;
	    using pointer = T*;
	    using reference = T&;

	    iterator() {}
	    bool  operator==(const iterator &other) const {
		return m == other.m && l1 == other.l1 && l2 == other.l2;
	    }

	    bool operator!=(const iterator &other) const {
		return m != other.m || l1 != other.l1 || l2 != other.l2;
	    }
	    iterator(extmap *m, l1_iter _l1, l2_iter _l2) {
		this->m = m;
		this->l1 = l1;
		this->l2 = l2;
	    }
	    reference operator*() const {
		return *l2;
	    }
	    pointer operator->() {
		return &(*l2);
	    }
	    iterator& operator++(int) {
		int i = l1 - m->the_map->begin();
		assert(l2 < m->lists[i]->end());
		it++;
		if (it == m->lists[i]->end()) {
		    if (i + 1 <  m->lists.size()) {
			i++;
			it = m->lists[i]->begin();
		    }
		}
		return *this;
	    }
	    
	};
	
	// never call this when empty
	std::pair<l1_iter,l2_iter> lower_bound(T_in base) {
	    local_xpair key = {.max = base, .list = nullptr};
	    auto iter1 = std::lower_bound(the_map.begin(), the_map.end(), key);
	    if (iter1 == the_map.end()) {
		iter1--;
		return std::make_pair(iter1, last()->list->end());
	    }
	    auto list = iter1->list;
	    T _key(base);
	    auto list_iter = std::lower_bound(list->begin(), list->end(), _key);

	    if (list_iter != list->begin()) {
		auto prev = list_iter - 1;
		if (prev->base() <= base && prev->limit() > base)
		    return std::make_pair(iter1, prev);
	    }
	    if (!next_to_last(iter1) && list_iter == list->end())
		return std::make_pair(next(iter1), next(iter1)->list->begin());
	    return std::make_pair(iter1, list_iter);
	}
	
	std::pair<l1_iter, l2_iter> _expand(l1_iter i, l2_iter it) {
	    if (i->list->size() >= _load * 2) {
		int j = it - i->list->begin();
		auto half = slice(i->list, _load);
		i->max = i->list->back().limit();
		//lists.insert(&lists[i+1], half);
		the_map.insert((local_xpair){.max = half->back().limit(),
			    .list = half});
		if (j >= _load) {
		    j -= _load;
		    i++;
		}
		it = i->list->begin()+j;
	    }
	    return std::make_pair(i, it);
	}

	void verify_max(void) {
	}

	// inserts just before iterator 'it'
	// returns pointer to inserted value
	std::pair<l1_iter, l2_iter> insert(l1_iter i, l2_iter it, T _e) {
	    if (it == i->list->end())
		i->max = _e.limit();
	    it = i->list->insert(it, _e);
	    return _expand(i, it);
	}
	
	// corresponds to sortedlist._delete
	//
	std::pair<l1_iter, l2_iter> _erase(l1_iter it1, l2_iter it) {
	    assert(it != it1->list->end());
	    it = it1->list->erase(it);
	    it1->max = it1->list->back().limit();
	    if (it1->list->size() > _load / 2)
		;
	    else if (the_map.size() > 1) {
		int j = it - it1->list->begin();
		auto pos_it = it1;
		auto prev_it = it1;
		if (pos_it == the_map.begin())
		    pos_it++;
		else {
		    prev_it--;
		    j += prev_it->list->size();
		}
		prev_it->list->insert(prev_it->list->end(),
				    pos_it->list->begin(), pos_it->list->end());
		prev_it->max = prev_it->list->back().limit();

		delete pos_it->list;
		the_map.erase(pos_it);
		std::tie(it1, it) = _expand(prev_it, prev_it->list->begin() + j);
	    }
	    else if (it1->list->size() > 0)
		it1->max = it1->list->back().limit();

	    if (it == it1->list->end() && !next_to_last(it1)) {
		it1++;
		it = it1->list->begin();
	    }
	    return std::make_pair(it1, it);
	}
	
	std::pair<l1_iter, l2_iter> begin(void) {
	    return std::make_pair(the_map.begin(), the_map.begin()->list->begin());
	}
	bool is_begin(l1_iter i, l2_iter it) {
	    return i == the_map.begin() && it == i->list->begin();
	}

	std::pair<l1_iter,l2_iter> end(void) {
	    auto i = the_map.end();
	    i--;
	    return std::make_pair(i, i->list->end());
	}
	bool is_end(l1_iter i, l2_iter it) {
	    return next_to_last(i) && it == i->list->end();
	}

	std::pair<l1_iter,l2_iter> decr(l1_iter i, l2_iter it) {
	    if (it == i->list->begin())
		return (i == the_map.begin()) ? begin() :
		    std::pair(prev(i), prev(i)->list->end() - 1);
	    return std::make_pair(i, it-1);
	}

	std::pair<l1_iter,l2_iter> incr(l1_iter i, l2_iter it) {
	    if (it+1 == i->list->end())
		return next_to_last(i) ?
		    end() : std::make_pair(next(i), next(i)->list->begin());
	    return std::make_pair(i, it+1);
	}
	
	std::pair<l1_iter,l2_iter> fix_it(T_in base, l1_iter i, l2_iter it) {
	    if (!is_begin(i, it)) {
		auto [prev_i, prev] = decr(i, it);
		if (prev->base() <= base && prev->limit() > base)
		    return std::make_pair(prev_i, prev);
	    }
	    return std::make_pair(i, it);
	}

	static bool adjacent(T left, T right) {
	    return left.limit() == right.base() &&
		left.s.ptr + (left.limit() - left.base()) == right.s.ptr;
	}

	void _update(T_in base, T_in limit, T_out e, bool trim, std::vector<T> *del) {
	    //= {.base = base, .limit = limit, .ext = e};
	    T _e(base, limit-base, e);

	    verify_max();

	    if (the_map.size() == 0) {
		auto vec = new l2_map();
		vec->reserve(_load);
		vec->push_back(_e);
		the_map.insert((local_xpair){.max = limit, .list = vec});
		return;
	    }

	    auto [i, it] = lower_bound(base);
	    assert(is_end(i, it) || it != i->list->end());
	    std::tie(i, it) = fix_it(base, i, it);
	    assert(is_end(i, it) || it != i->list->end());

	    verify_max();

	    if (!is_end(i,it)) {
		if (it->base() < base && it->limit() > limit) {
		    // we bisect an extent
		    //   [-----------------]       *it          _new
		    //          [+++++]       -> [-----][+++++][----]
		    //
		    if (del != nullptr) {
			T _old(base,		    // base
			       limit - base,	    // len
			       it->s.ptr + (base - it->base())); // ptr
			del->push_back(_old);
		    }
		    T _new(limit, /* base */
			   it->limit() - limit, /* len */
			   it->s.ptr + (limit - it->base()));
		    it->limit(base);
		    i->max = i->list->back().limit();
		    std::tie(i, it) = incr(i, it);
		    assert(is_end(i, it) || it != i->list->end());
		    std::tie(i, it) = insert(i, it, _new);
		    assert(is_end(i, it) || it != i->list->end());
		    verify_max();

		}
		// left-hand overlap
		//   [---------]
		//       [++++++++++]  ->  [----][++++++++++]
		//
		else if (it->base() < base && it->limit() > base) {
		    if (del != nullptr) {
			T _old(base,		    // base
			       it->limit() - base,  // len
			       it->s.ptr + (base - it->base())); // ptr
			del->push_back(_old);
		    }
		    it->limit(base);
		    i->max = i->list->back().limit();
		    std::tie(i, it) = incr(i, it);
		    assert(is_end(i, it) || it != i->list->end());
		    verify_max();
		}

		// erase any extents fully overlapped
		//       [----] [---] 
		//   [+++++++++++++++++] -> [+++++++++++++++++]
		//
		while (!is_end(i,it)) {
		    assert(it != i->list->end());
		    if (it->base() >= base && it->limit() <= limit) {
			if (del != nullptr) 
			    del->push_back(*it);
			std::tie(i,it) = _erase(i, it);
			assert(is_end(i, it) || it != i->list->end());
			verify_max();
		    } else
			break;
		    assert(is_end(i, it) || it != i->list->end());
		}
		// update right-hand overlap
		//        [---------]
		//   [++++++++++]        -> [++++++++++][----]
		//
		if (!is_end(i, it) && limit > it->base()) {
		    if (del != nullptr) {
			T _old(it->base(),	   // base
			       limit - it->base(),  // len
			       it->s.ptr);
			del->push_back(_old);
		    }
		    it->s.ptr += (limit - it->base());
		    it->base(limit);
		    verify_max();
		}
	    }

	    // insert before 'it'
	    if (!trim) {
		auto [p, prev] = decr(i, it);
		if (!is_begin(i, it) && adjacent(*prev, _e)) {
		    prev->limit(limit);
		    p->max = p->list->back().limit();
		    if (!is_end(i, it) && adjacent(*prev, *it)) {
			prev->limit(it->limit());
			p->max = p->list->back().limit();
			_erase(i, it);
			verify_max();
			return;
		    }
		}
		else if (!is_end(i, it) && adjacent(_e, *it)) {
		    it->s.ptr += (base - limit); // subtract
		    it->base(base);
		}
		else {
		    insert(i, it, _e);
		    verify_max();
		}
	    }
	}

	// returns iterator pointing to one of:
	// - extent containing @base
	// - lowest extent with base > @base
	// - end()
	std::pair<l1_iter,l2_iter> _lookup(T_in base) {
	    auto [i, it] = lower_bound(base);
	    if (is_end(i, it)) 
		return std::pair(i, it);
	    auto [base1, limit1, ext1] = *it;

	    // 222222222  1111111111
	    // |       |  |        +- limit1
	    // |       |  +- base1
	    // |       +- limit2
	    // +- base2
	    if (base1 > base && !is_begin(i, it)) {
		auto [i2, it2] = decr(i, it);
		auto [base2, limit2, ext2] = *it2;
		if (base < limit2)
		    return std::make_pair(i2, it2);
	    }
	    return std::make_pair(i, it);
	}

    public:
	int size() {
	    int sum = 0;
	    for (auto entry : the_map)
		sum += entry.list->size();
	    return sum;
	}

	int capacity() {
	    int sum = 0;
	    for (auto entry : the_map)
		sum += entry.list->capacity();
	    return sum;
	}

	void update(T_in base, T_in limit, T_out e) {
	    _update(base, limit, e, false, nullptr);
	}

	void trim(T_in base, T_in limit, T_out e) {
	    _update(base, limit, e, true, nullptr);
	}

	
    };

    // template <class T, class T_in, class T_out>
    typedef extmap<lba2obj,int64_t,obj_offset> objmap;
    typedef extmap<obj2lba,obj_offset,int64_t> cachemap;
    typedef extmap<lba2buf,int64_t,sector_ptr> bufmap;
}

