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
#include <set>
#include <tuple>
#include <cassert>

// LBA -> backend:   map<lba_len,obj_offset>
// backend -> cache: map<obj_offset_len,lba>
namespace extmap {

    struct obj_offset {
	int64_t obj    : 36;
	int64_t offset : 28;	// 128GB
    public:
	obj_offset operator+=(int val) {
	    offset += val;
	    return *this;
	}
	bool operator<(const obj_offset other) const {
	    return (obj == other.obj) ? (offset < other.offset) : (obj < other.obj);
	}
    };

    // These are the three map types we support. There's probably a way to 
    // do this with a template, but I don't think it's worth the effort
    //
    typedef char sector_t[512];
    
    struct _lba2buf {
	int64_t   base : 40;
	int64_t   len  : 24;
	sector_t *ptr;
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

    template <class T> 
    struct _extent {
	T s;
    public:
	_extent(int64_t _base, int64_t _len, obj_offset _ptr) {
	    s.base = _base;
	    s.len = _len;
	    s.ptr = _ptr;
	}
	int64_t base(void) { return s.base; }
	int64_t limit(void) {return s.base + s.len; }
	
	_extent operator+=(int val) {
	    s.ptr += val;
	    return *this;
	}
	bool operator<(const _extent &other) const {
	    return s.base < other.s.base;
	}
    };

    typedef _extent<_lba2buf> lba2buf;
    typedef _extent<_obj2lba> obj2lba;
    typedef _extent<_lba2obj> lba2obj;

    template <class T>
    struct x_pair {
	mutable int64_t max;
	std::vector<T> *list;
    public:
	bool operator<(const x_pair &other) const {
	    return max < other.max;
	}
    };

    template <class T>		// maybe ", int load"??
    struct extmap {
	static const int _load = 256;

	typedef          std::set<x_pair<T>> l1_map;
	typedef typename l1_map::iterator    l1_iter;
	typedef          std::vector<T>      l2_map;
	typedef typename l2_map::iterator    l2_iter;
	
	l1_map the_map;
	//std::set<x_pair<T>> the_map;

	static std::vector<T> *_trim(std::vector<T> *A, int len) {
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

	// never call this when empty
	std::pair<l1_iter,l2_iter> lower_bound(int64_t base) {
	    x_pair<T> key = {.max = base, .list = nullptr};
	    auto iter1 = std::lower_bound(the_map.begin(), the_map.end(), key);
	    if (iter1 == the_map.end()) {
		iter1--;
		return std::make_pair(iter1, last()->list->end());
	    }
	    auto list = iter1->list;
	    auto list_iter = std::lower_bound(list->begin(), list->end(),
					      (T){.base = base});
	    if (list_iter != list->begin()) {
		auto prev = list_iter - 1;
		if (prev->base <= base && prev->limit > base)
		    return std::make_pair(iter1, prev);
	    }
	    if (!next_to_last(iter1) && list_iter == list->end())
		return std::make_pair(next(iter1), next(iter1)->list->begin());
	    return std::make_pair(iter1, list_iter);
	}
	
	std::pair<l1_iter, l2_iter>
	_expand(l1_iter i, l2_iter it) {
	    if (i->list->size() >= _load * 2) {
		int j = it - i->list->begin();
		auto half = _trim(i->list, _load);
		i->max = i->list->back().limit;
		//lists.insert(&lists[i+1], half);
		the_map.insert((x_pair<T>){.max = half->back().limit,
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
		i->max = _e.limit;
	    it = i->list->insert(it, _e);
	    return _expand(i, it);
	}
	
	// corresponds to sortedlist._delete
	//
	std::pair<l1_iter, l2_iter> _erase(l1_iter it1, l2_iter it) {
	    assert(it != it1->list->end());
	    it = it1->list->erase(it);
	    it1->max = it1->list->back().limit;
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
		prev_it->max = prev_it->list->back().limit;

		delete pos_it->list;
		the_map.erase(pos_it);
		std::tie(it1, it) = _expand(prev_it, prev_it->list->begin() + j);
	    }
	    else if (it1->list->size() > 0)
		it1->max = it1->list->back().limit;

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
	
	std::pair<l1_iter,l2_iter> fix_it(int64_t base, l1_iter i, l2_iter it) {
	    if (!is_begin(i, it)) {
		auto [prev_i, prev] = decr(i, it);
		if (prev->base <= base && prev->limit > base)
		    return std::make_pair(prev_i, prev);
	    }
	    return std::make_pair(i, it);
	}

	static bool adjacent(T left, T right) {
	    return left.limit == right.base &&
		left.ext + (left.limit - left.base) == right.ext;
	}

	void _update(int64_t base, int64_t limit, T e, bool trim) {
	    //= {.base = base, .limit = limit, .ext = e};
	    T _e(base, limit-base, e.s.ptr);

	    verify_max();

	    if (the_map.size() == 0) {
		auto vec = new l2_map();
		vec->reserve(_load);
		vec->push_back(_e);
		the_map.insert((x_pair<T>){.max = limit, .list = vec});
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
		    T _new = {.base = limit, .limit = it->limit,
			      .ext = it->ext + (limit - it->base)};
		    it->limit = base;
		    i->max = i->list->back().limit;
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
		else if (it->base < base && it->limit > base) {
		    it->limit = base;
		    i->max = i->list->back().limit;
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
		    if (it->base >= base && it->limit <= limit) {
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
		if (!is_end(i, it) && limit > it->base) {
		    it->ext += (limit - it->base);
		    it->base = limit;
		    verify_max();
		}
	    }

	    // insert before 'it'
	    if (!trim) {
		auto [p, prev] = decr(i, it);
		if (!is_begin(i, it) && adjacent(*prev, _e)) {
		    prev->limit = limit;
		    p->max = p->list->back().limit;
		    if (!is_end(i, it) && adjacent(*prev, *it)) {
			prev->limit = it->limit;
			p->max = p->list->back().limit;
			_erase(i, it);
			verify_max();
			return;
		    }
		}
		else if (!is_end(i, it) && adjacent(_e, *it)) {
		    it->ext += (base - limit); // subtract
		    it->base = base;
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
	std::pair<l1_iter,l2_iter> _lookup(int64_t base) {
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

	void update(int64_t base, int64_t limit, T e) {
	    _update(base, limit, e, false);
	}

	void trim(int64_t base, int64_t limit, T e) {
	    _update(base, limit, e, true);
	}
    };
}

