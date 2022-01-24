#include <stdlib.h>
#include <assert.h>
#include "extent.cc"
#include <vector>

#include <random>
std::mt19937 gen;

std::pair<int64_t,int64_t> rnd_pair(int64_t max)
{
    std::uniform_int_distribution<> distrib(0, max-2);
    int64_t base = distrib(gen);
    std::geometric_distribution<> d(10);
    int64_t len = d(gen) + 1;
    int64_t limit = std::min(base+len, max-1);
    return std::make_pair(base, limit);
}

std::vector<std::pair<int64_t,int64_t>> *rnd_list(int64_t max, int n)
{
    auto v = new std::vector<std::pair<int64_t,int64_t>>;
    for (int i = 0; i < n; i++)
	v->push_back(rnd_pair(max));
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

std::vector<extmap::obj_offset> *flatten(std::vector<std::pair<int64_t,int64_t>> *writes)
{
    auto v = new std::vector<extmap::lba2obj>;
    for (auto [base, limit] : *writes) {
	extmap::lba2obj l2o(base, limit-base, (extmap::obj_offset){0, base});
	v->push_back(l2o);
    }
    auto val = flatten(v);
    delete v;
    return val;
}
    
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
	while ((*writes)[i].obj != -1 && i < writes->size()) {
	    limit = i+1;
	    i++;
	}
	extmap::lba2obj l2o(base, limit-base, (extmap::obj_offset){obj, base});
	v->push_back(l2o);
    }
    return v;
}

void addit(extmap::objmap *m, int64_t base, int64_t limit)
{
    extmap::obj_offset ptr = {0, base};
    m->update(base, limit, ptr);
}

void test_ptr(extmap::objmap *map)
{
    for (auto it = map->begin(); it != map->end(); it++) {
	auto [base, limit, ptr] = it->vals();
	assert(base == ptr.offset);
    }
}

// verify that merge works at different positions in the map
void test_seqw_800(void)
{
    for (int k = 2; k < 600; k++) {
	extmap::objmap map;
	for (int i = 0; i < 8000; i += 10) {
	    addit(&map, i, i+5);
	    test_ptr(&map);
	    
//	for (int j = 0; j < map.lists.size(); j++) 
//	    printf(" %ld", map.lists[j]->size());
//	printf("\n");
	}
    
	int i = 0;
	for (auto it = map.begin(); it != map.end(); it++, i += 10) {
	    auto [base, limit, ptr] = it->vals();
	    assert(base == i);
	    assert(limit == i+5);
	    assert(ptr.offset == i);
	}
    

	assert(map.lists.size() == 3);
	assert(map.lists[0]->size() == 256);
	addit(&map, k*10+5, k*10+10);

	auto it = map.begin();
	for (i = 0; i < k; i++, it++) {
	    auto [base, limit, ptr] = it->vals();
	    assert(base == i*10 && limit == i*10+5 && ptr.offset == base);
	}

	auto [base, limit, ptr] = it->vals();
	assert(base == k*10 && limit == k*10+15 && ptr.offset == k*10);
	it++;
	for (i = k+2; i < 800; i++, it++) {
	    auto [base, limit, ptr] = it->vals();
	    assert(base == i*10 && limit == i*10+5 && ptr.offset == base);
	}
    }
    printf("seqw_800: OK\n");
}

void test_17w_800(void)
{
    extmap::objmap map;
    for (int i = 0, j = 0; i < 800; i++, j = (j+17)%800) {
	addit(&map, j*10, j*10+5);
	test_ptr(&map);
    }
		
    int i = 0;
    for (auto it = map.begin(); it != map.end(); it++, i += 10) {
	auto [base, limit, ptr] = it->vals();
	assert(base == i);
	assert(limit == i+5);
	assert(ptr.offset == i);
    }
    assert(i == 8000);
    printf("17w_800: OK\n");
//    printf("%d %d %d\n", (int) map.lists[0]->size(), (int)map.lists.size(),
//	   (int)map.lists[1]->size());
}

void test_rand_800_10k(void)
{
    extmap::objmap map;
    auto writes = rnd_list(1000, 80);
    int i = 0;
    for (auto [base, limit] : *writes) {
	addit(&map, base, limit);
	test_ptr(&map);
	i++;
    }
    auto flat = flatten(writes);
    auto merged = merge(flat);

    auto merged_it = merged->begin();
    auto map_it = map.begin();
    while (merged_it != merged->end()) {
	assert(map_it != map.end());
	auto [base, limit, ptr] = map_it->vals();
	auto [mbase, mlimit, mptr] = merged_it->vals();
	printf("map: %lld..%lld %lld in: %lld..%lld %lld\n", base, limit, ptr.offset,
	       mbase, mlimit, mptr.offset);
	printf("val %d\n", (mbase == base && mlimit == limit && mptr == ptr));

	assert(mbase == base && mlimit == limit && mptr == ptr);
	merged_it++;
	map_it++;
    }
    assert(map_it == map.end());
    
    printf("rand_800_10k: OK\n");
}

int main()
{
    //test_seqw_800();
    //test_17w_800();
    test_rand_800_10k();

#if 0
    extmap::objmap   map1;
    extmap::cachemap map2;
    extmap::bufmap   map3;

    for (int i = 0; i < 100000; i++) {
	int64_t base = random() % 1000000;
	int len = random() % 300;
	if (len > 32)
	    int len = random() % 300;
	if (len > 128)
	    int len = random() % 300;
	int64_t offset = random() % 1000000;
	extmap::obj_offset oo = {0, offset};
	map1.update(base, base+len, oo);
    }
    
    extmap::obj_offset e = {1, 2};
    map1.update(10, 20, e);
    map2.update(e, e+30, 1000);
    char b[512];
    map3.update(40, 41, b);
    auto it = map1.lookup(100);
#endif
}
