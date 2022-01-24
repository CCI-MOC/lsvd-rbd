//
// file:        unit-test.cc
// description: unit tests for extent.cc (first set?)
//

#include <stdlib.h>
#include <assert.h>
#include "extent.cc"
#include <vector>


// test that ptr.offset == base in all cases
//
void test_ptr(extmap::objmap *map)
{
    return;
    for (auto it = map->begin(); it != map->end(); it++) {
	auto [base, limit, ptr] = it->vals();
	assert(base == ptr.offset);
    }
}

// test 1 - insert sequentially, verify
//
void test_1_seq(void)
{
    extmap::objmap map;
    int max = 800;
    
    for (int i = 0; i < max; i++) {
	int base = i*10, limit = base + 5;
	extmap::obj_offset ptr = {0, base};
	map.update(base, limit, ptr);
	test_ptr(&map);
    }
    
    int i = 0;
    for (auto it = map.begin(); it != map.end(); it++, i++) {
	auto [base, limit, ptr] = it->vals();
	assert(base == i*10 && limit == i*10 + 5 && ptr.obj == 0 && ptr.offset == base);
    }

    for (i = 0; i < max; i++) {
	int base = i*10 + 2, limit = base + 6;
	for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, _ptr] = it->vals(base, limit);
	    assert(_base == base && _limit == base+3 && _ptr.offset == base);
	}
    }
    printf("%s: OK\n", __func__);
}

// same as the previous test, but order inserts mod 17
//
void test_2_mod17(void)
{
    extmap::objmap map;
    int max = 800;
    
    for (int i = 0, j = 0; i < max; i++, j = (j+17) % max) {
	int base = j*10, limit = base + 5;
	extmap::obj_offset ptr = {0, base};
	map.update(base, limit, ptr);
	test_ptr(&map);
    }
    
    int i = 0;
    for (auto it = map.begin(); it != map.end(); it++, i++) {
	auto [base, limit, ptr] = it->vals();
	assert(base == i*10);
	assert(limit == i*10+5);
	assert(ptr.offset == base);
    }
    assert(i == max);
    
    printf("%s: OK\n", __func__);
}

// verify that merge works at different positions in the map
//
void test_3_seq_merge(void)
{
    int max = 800, merge_min = 2, merge_max = max-5;
    
    for (int k = merge_min; k < merge_max; k++) {
	extmap::objmap map;
	for (int i = 0; i < max; i++) {
	    int base = i*10, limit = base + 5;
	    extmap::obj_offset ptr = {0, base};
	    map.update(base, limit, ptr);
	    test_ptr(&map);
	}
	    
	int i = 0;
	for (auto it = map.begin(); it != map.end(); it++, i++) {
	    auto [base, limit, ptr] = it->vals();
	    assert(base == i*10 && limit == i*10+5 && ptr.offset == base);
	}

	{
	    int base = k*10, limit = base + 15;
	    extmap::obj_offset ptr = {0, base};
	    map.update(base, limit, ptr);
	    test_ptr(&map);
	}

	auto it = map.begin();
	for (i = 0; i < k; i++, it++) {
	    auto [base, limit, ptr] = it->vals();
	    assert(base == i*10 && limit == i*10+5 && ptr.offset == base);
	}

	auto [base, limit, ptr] = it->vals();
	assert(base == k*10 && limit == k*10+15 && ptr.offset == k*10);
	it++;
	for (i = k+2; i < max; i++, it++) {
	    auto [base, limit, ptr] = it->vals();
	    assert(base == i*10 && limit == i*10+5 && ptr.offset == base);
	}
    }
    printf("%s: OK\n", __func__);
}

// various infrastructure for random tests
//
#include <random>
std::mt19937 gen;

// create a random vector of @n extents in [0..max)
//
std::vector<extmap::lba2obj> *rnd_extents(int64_t max, int n, bool rnd_obj, bool rnd_offset)
{
    std::uniform_int_distribution<> unif(0, max-2);
    std::geometric_distribution<> geo(0.05); // mean 20
    auto v = new std::vector<extmap::lba2obj>;

    for (int i = 0; i < n; i++) {
	int64_t base = unif(gen);
	int64_t len = geo(gen) + 1;
	int64_t limit = std::min(base+len, max-1);

	int64_t obj = 0, offset = base;
	if (rnd_obj) 
	    obj = unif(gen);
	if (rnd_offset)
	    offset = unif(gen);
	extmap::obj_offset oo = {obj, offset};
	extmap::lba2obj l2o(base, limit-base, oo);
	v->push_back(l2o);
    }
    return v;
}

// returns a vector of obj_offset indexed by LBA. missing entries are
// flagged by obj==-1. Last element of array is guaranteed to be missing
//
std::vector<extmap::obj_offset> *flatten(std::vector<extmap::lba2obj> *writes)
{
    int64_t max = 0;
    for (auto w : *writes) 
	max = (w.limit() > max) ? w.limit() : max;
    auto v = new std::vector<extmap::obj_offset>;
    for (int i = 0; i < max+2; i++)
	v->push_back((extmap::obj_offset){-1, 0});

    for (auto w : *writes) {
	int64_t i, j, obj = w.s.ptr.obj;
	for (i = w.base(), j = w.s.ptr.offset; i < w.limit(); i++, j++) 
	    (*v)[i] = (extmap::obj_offset){obj, j};
    }
    
    return v;
}

// Merge a flattened vector back into a vector of extents. Should be in the
// same order as we iterate them from the map.
//
std::vector<extmap::lba2obj> *merge(std::vector<extmap::obj_offset> *writes)
{
    int64_t base = -1, limit = -1;
    auto v = new std::vector<extmap::lba2obj>;
    int i = 0;
    while (i < writes->size()) {
	while ((*writes)[i].obj == -1 && i < writes->size()) {
	    base = i+1;
	    i++;
	}
	auto obj = (*writes)[i].obj;
	auto offset = (*writes)[i].offset;
	while ((*writes)[i].obj != -1 && (*writes)[i].obj == obj && i < writes->size()) {
	    limit = i+1;
	    i++;
	}
	if (limit > base) {
	    extmap::lba2obj l2o(base, limit-base, (extmap::obj_offset){obj, offset});
	    v->push_back(l2o);
	}
	base = limit;
    }
    return v;
}

void test_4_rand(void)
{
    int max = 8000, n = 2000;
    auto writes = rnd_extents(max, n, false, false);

    extmap::objmap map;
    for (auto l2o : *writes) {
	auto [base, limit, ptr] = l2o.vals();
	map.update(base, limit, ptr);
	test_ptr(&map);
    }

    auto flat = flatten(writes);
    auto merged = merge(flat);

    auto merged_it = merged->begin();
    auto map_it = map.begin();
    while (merged_it != merged->end()) {
	assert(map_it != map.end());
	auto [base, limit, ptr] = map_it->vals();
	auto [mbase, mlimit, mptr] = merged_it->vals();
#if 0
	printf("map: %lld..%lld %lld in: %lld..%lld %lld\n", base, limit, ptr.offset,
	       mbase, mlimit, mptr.offset);
	printf("val %d\n", (mbase == base && mlimit == limit && mptr == ptr));
#endif
	assert(mbase == base && mlimit == limit && mptr == ptr);
	merged_it++;
	map_it++;
    }
    assert(map_it == map.end());
    
    printf("%s: OK\n", __func__);
}

void _test_5_rand(int max, int n)
{
    auto writes = rnd_extents(max, n, false, false);

    extmap::objmap map;
    for (auto l2o : *writes) {
	auto [base, limit, ptr] = l2o.vals();
	map.update(base, limit, ptr);
	test_ptr(&map);
    }

    auto flat = flatten(writes);
    auto merged = merge(flat);

    auto merged_it = merged->begin();
    auto map_it = map.begin();
    while (merged_it != merged->end()) {
	assert(map_it != map.end());
	auto [base, limit, ptr] = map_it->vals();
	auto [mbase, mlimit, mptr] = merged_it->vals();
#if 0
	printf("map: %lld..%lld %lld in: %lld..%lld %lld\n", base, limit, ptr.offset,
	       mbase, mlimit, mptr.offset);
	printf("val %d\n", (mbase == base && mlimit == limit && mptr == ptr));
#endif
	assert(mbase == base && mlimit == limit && mptr == ptr);
	merged_it++;
	map_it++;
    }
    assert(map_it == map.end());
}

// test heavily loaded map
//
void test_5_rand(void)
{
    _test_5_rand(80000, 20000);
    printf("%s: OK\n", __func__);
}

// and sparse map
//
void test_6_rand(void)
{
    _test_5_rand(800000, 20000);
    printf("%s: OK\n", __func__);
}    

// test 7 - verify lookup where base overlaps prior extent
//
void test_7_lookup(void)
{
    int max = 800;

    for (int k = 5; k < max-5; k++) {
      extmap::objmap map;
      for (int i = 0; i < max; i++) {
	int base = i*10, limit = base + 5;
	extmap::obj_offset ptr = {0, base};
	map.update(base, limit, ptr);
	test_ptr(&map);
      }

      auto it = map.lookup(k*10 + 3);
      auto [base, limit, ptr] = it->vals(k*10+3, k*10+8);
      assert(base == k*10+3 && ptr.offset == k*10+3 && limit == k*10+5);
    }
    printf("%s: OK\n", __func__);
}

// like the previous random test, except that we knock out a whole bunch 
void test_8_rand(void)
{
    int max = 2000000, n = 200000;
    auto writes = rnd_extents(max, n, true, true);

    int64_t base = max/10, limit = max - base;
    extmap::lba2obj l2o(base, limit, (extmap::obj_offset){0, base});
    writes->push_back(l2o);
    
    extmap::objmap map;
    for (auto l2o : *writes) {
	auto [base, limit, ptr] = l2o.vals();
	map.update(base, limit, ptr);
	test_ptr(&map);
    }

    auto flat = flatten(writes);
    auto merged = merge(flat);

    auto merged_it = merged->begin();
    auto map_it = map.begin();
    while (merged_it != merged->end()) {
	assert(map_it != map.end());
	auto [base, limit, ptr] = map_it->vals();
	auto [mbase, mlimit, mptr] = merged_it->vals();
	assert(mbase == base && mlimit == limit && mptr == ptr);
	merged_it++;
	map_it++;
    }
    assert(map_it == map.end());
    
    printf("%s: OK\n", __func__);
}

void test_9_delete(void)
{
    extmap::objmap map;
    int max = 800;
    
    for (int i = 0; i < max; i++) {
	int base = i*10, limit = base + 5;
	extmap::obj_offset ptr = {0, base};
	map.update(base, limit, ptr);
	test_ptr(&map);
    }
    
    std::vector<extmap::lba2obj> v;
    extmap::obj_offset ptr = {0, 2222};
    
    map.update(252, 328, ptr, &v);

    // should knock out 252-255, 260-265, ... 320..325
    int i = 0;
    for (auto it = map.begin(); it != map.end(); it++, i++) {
	auto [base, limit, ptr] = it->vals();
	if (i < 25) 
	    assert(base == i*10 && limit == i*10 + 5 &&
		   ptr.obj == 0 && ptr.offset == base);
	else if (i == 25)
	    assert(base == 250 && limit == 252);
	else if (i == 26)
	    assert(base == 252 && limit == 328 && ptr.offset == 222);
	else if (i > 26)
	    assert (base == (i+7)*10 && limit == (i+7)*10 + 5 &&
		    ptr.offset == base);
    }

    assert(v.size() == 8);
    auto it = v.begin();
    auto [_base, _limit, _ptr] = it->vals();
    assert(_base == 252 && _limit == 255 && _ptr.offset == 252);
    it++;
    
    for (int i = 0; it != v.end(); it++, i++) {
	auto [base, limit, ptr] = it->vals();
	assert(base == 255 + i*10 && limit == base+5 && ptr.offset == base);
    }
    
    printf("%s: OK\n", __func__);
}


// only thing left to 

int main()
{
    test_1_seq();
    test_2_mod17();
    test_3_seq_merge();
    test_4_rand();
    test_5_rand();
    test_6_rand();
    test_7_lookup();
    test_8_rand();
}
