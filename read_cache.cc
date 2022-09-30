/*
 * file:        read_cache.cc
 * description: implementation of read cache
 *              the read cache is:
 *                       * 1. indexed by obj/offset[*], not LBA
 *                       * 2. stores aligned 64KB blocks
 *                       * [*] offset is in units of 64KB blocks
 * author:      Peter Desnoyers, Northeastern University
 *              Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <unistd.h>
#include <stdint.h>

#include <shared_mutex>
#include <mutex>
#include <thread>
#include <atomic>
#include <cassert>


#include <queue>
#include <map>
#include <stack>
#include <vector>

#include <random>

#include "smartiov.h"
#include "extent.h"

#include "journal.h"

#include "base_functions.h"
#include "backend.h"
#include "misc_cache.h"
#include "io.h"
#include "translate.h"
#include "read_cache.h"
#include "objname.h"

class read_cache_impl : public read_cache {
    
    std::mutex m;
    std::map<extmap::obj_offset,int> map;

    j_read_super       *super;
    extmap::obj_offset *flat_map;
    objmap             *omap;
    translate          *be;
    int                 fd;
    size_t              dev_max;
    backend            *io;
    
    int               unit_sectors;
    std::vector<int>  free_blks;
    bool              map_dirty = false;


    // new idea for hit rate - require that sum(backend reads) is no
    // more than 2 * sum(read sectors) (or 3x?), using 64bit counters 
    //
    struct {
	int64_t       user = 1000; // hack to get test4/test_2_fakemap to work
	int64_t       backend = 0;
    } hit_stats;
    
    thread_pool<int> misc_threads; // eviction thread, for now
    bool             nothreads = false;	// for debug

    /* if map[obj,offset] = n:
     *   in_use[n] - not eligible for eviction
     *   written[n] - safe to read from cache
     *   buffer[n] - in-memory data for block n
     *   pending[n] - continuations to invoke when buffer[n] becomes valid
     * buf_loc - FIFO queue of {n | buffer[n] != NULL}
     */
    sized_vector<std::atomic<int>>   in_use;
    sized_vector<char>               written; // can't use vector<bool> here
    sized_vector<char*>              buffer;
    sized_vector<std::vector<void*>> pending;
    std::queue<int>    buf_loc;
    
    io_context_t ioctx;
    std::thread e_io_th;
    bool e_io_running = false;
    
    /* possible CLOCK implementation - queue holds <block,ojb/offset> 
     * pairs so that we can evict blocks without having to remove them 
     * from the CLOCK queue
     */
    sized_vector<char> a_bit;
#if 0
    sized_vector<int>  block_version;
    std::queue<std::pair<int,extmap::obj_offset>> clock_queue;
#endif
    
    /* evict 'n' blocks - random replacement
     */
// evict :	Frees n number of blocks and erases oo from the map
    void evict(int n);

    void evict_thread(thread_pool<int> *p);
    
    char *get_cacheline_buf(int n); /* TODO: document this */

public:
    read_cache_impl(uint32_t blkno, int _fd, bool nt,
		    translate *_be, objmap *_om, backend *_io);
    ~read_cache_impl();
    
    std::pair<size_t,size_t> async_read(size_t offset, char *buf, size_t len,
					void (*cb)(void*), void *ptr);

    /* debugging. 
     */

    /* get superblock, flattened map, vector of free blocks, extent map
     * only returns ones where ptr!=NULL
     */
    void get_info(j_read_super **p_super, extmap::obj_offset **p_flat, 
		  std::vector<int> **p_free_blks,
                  std::map<extmap::obj_offset,int> **p_map);

    void do_add(extmap::obj_offset unit, char *buf); /* TODO: document */
    void do_evict(int n);       /* TODO: document */
    void write_map(void);
};

/* factory function so we can hide implementation
 */
read_cache *make_read_cache(uint32_t blkno, int _fd, bool nt, translate *_be,
			    objmap *_om, backend *_io) {
    return new read_cache_impl(blkno, _fd, nt, _be, _om, _io);
}

/* constructor - allocate, read the superblock and map, start threads
 */
read_cache_impl::read_cache_impl(uint32_t blkno, int _fd, bool nt,
				 translate *_be, objmap *_om,
				 backend *_io) : misc_threads(&m) {
    omap = _om;
    be = _be;
    fd = _fd;
    io = _io;
    nothreads = nt;
    
    dev_max = getsize64(fd);
    char *buf = (char*)aligned_alloc(512, 4096);
    if (pread(fd, buf, 4096, 4096L*blkno) < 4096)
	throw_fs_error("rcache3");
    super = (j_read_super*)buf;

    assert(super->unit_size == 128); // 64KB, in sectors
    unit_sectors = super->unit_size; // todo: fixme

    int oos_per_pg = 4096 / sizeof(extmap::obj_offset);
    assert(div_round_up(super->units, oos_per_pg) == super->map_blocks);

    flat_map = (extmap::obj_offset*)aligned_alloc(512, super->map_blocks*4096);
    if (pread(fd, (char*)flat_map, super->map_blocks*4096,
	      super->map_start*4096L) < 0)
	throw_fs_error("rcache2");

    for (int i = 0; i < super->units; i++) {
	if (flat_map[i].obj != 0) 
	    map[flat_map[i]] = i;
	else 
	    free_blks.push_back(i);
    }

    in_use.init(super->units);
    written.init(super->units);
    buffer.init(super->units);
    pending.init(super->units);
    a_bit.init(super->units);

    map_dirty = false;

    misc_threads.pool.push(std::thread(&read_cache_impl::evict_thread,
				       this, &misc_threads));

    io_queue_init(64, &ioctx);
    e_io_running = true;
    const char *name = "read_cache_cb";
    e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
}

read_cache_impl::~read_cache_impl() {
    misc_threads.stop();	// before we free anything threads might touch
	
    free((void*)flat_map);
    for (auto i = 0; i < super->units; i++)
	if (buffer[i] != NULL)
	    free(buffer[i]);
    free((void*)super);

    e_io_running = false;
    e_io_th.join();
    io_queue_release(ioctx);
}

#if 0
static std::random_device rd; // to initialize RNG
static std::mt19937 rng(rd());
#else
static std::mt19937 rng(17);      // for deterministic testing
#endif

/* evict 'n' blocks from cache, using random eviction
 */
void read_cache_impl::evict(int n) {
    // assert(!m.try_lock());       // m must be locked
    std::uniform_int_distribution<int> uni(0,super->units - 1);
    for (int i = 0; i < n; i++) {
	int j = uni(rng);
	while (flat_map[j].obj == 0 || in_use[j] > 0)
	    j = uni(rng);
	auto oo = flat_map[j];
	flat_map[j] = (extmap::obj_offset){0, 0};
	map.erase(oo);
	free_blks.push_back(j);
    }
}

void read_cache_impl::evict_thread(thread_pool<int> *p) {
    auto wait_time = std::chrono::milliseconds(500);
    auto t0 = std::chrono::system_clock::now();
    auto timeout = std::chrono::seconds(2);

    std::unique_lock<std::mutex> lk(m);

    while (p->running) {
	p->cv.wait_for(lk, wait_time);
	if (!p->running)
	    return;

	int n = 0;
	if ((int)free_blks.size() < super->units / 16)
	    n = super->units / 4 - free_blks.size();
	if (n)
	    evict(n);

	if (!map_dirty)     // free list didn't change
	    continue;

	/* write the map (a) immediately if we evict something, or 
	 * (b) occasionally if the map is dirty
	 */
	auto t = std::chrono::system_clock::now();
	if (n > 0 || (t - t0) > timeout) {
	    lk.unlock();
	    write_map();
	    t0 = t;
	    lk.lock();
	    map_dirty = false;
	}
    }
}

    /* state machine for block obj,offset can be represented by the tuple:
     *  map=n - i.e. exists(n) | map[obj,offset] = n
     *  in_use[n] - 0 / >0
     *  written[n] - n/a, F, T
     *  buffer[n] - n/a, NULL, <p>
     *  pending[n] - n/a, [], [...]
     *
     * if not cached                          -> {!map, n/a}
     * first read will:
     *   - add to map
     *   - increment in_use
     *   - launch read                        -> {map=n, >0, F, NULL, []}
     * following reads will 
     *   queue lambdas to copy from buffer[n] -> {map=n, >0, F, NULL, [..]}
     * read complete will:
     *   - set buffer[n]
     *   - invoke lambdas from pending[*]
     *   - launch write                       -> {map=n, >0, F, <p>, []}
     * write complete will:
     *   - set 'written' to true              -> {map=n, >0, T, <p>, []}
     * eviction of buffer will:
     *   - decr in_use
     *   - remove buffer                      -> {map=n, 0, NULL, []}
     * further reads will temporarily increment in_use
     * eviction will remove from map:         -> {!map, n/a}
     */


/* TODO: WTF is this?
 */
char *read_cache_impl::get_cacheline_buf(int n) {
    char *buf;
    int len = 65536;
    const int maxbufs = 48;
    if (buf_loc.size() < maxbufs) {
	buf = (char*)aligned_alloc(512, len);
	memset(buf, 0, len);
    }
    else {
	int j = buf_loc.front();
	buf_loc.pop();
	//printf("stealing %d\n", j);
	assert(buffer[j] != NULL);
	buf = buffer[j];
	buffer[j] = NULL;
	in_use[j]--;
    }
    buf_loc.push(n);
    assert(buf != NULL);
    return buf;
}

std::pair<size_t,size_t>
read_cache_impl::async_read(size_t offset, char *buf, size_t len,
			    void (*cb)(void*), void *ptr) {
    sector_t base = offset/512, sectors = len/512, limit = base+sectors;
    size_t skip_len = 0, read_len = 0;
    extmap::obj_offset oo = {0, 0};

    std::shared_lock lk(omap->m);
    auto it = omap->map.lookup(base);
    if (it == omap->map.end() || it->base() >= limit)
	skip_len = len;
    else {
	auto [_base, _limit, _ptr] = it->vals(base, limit);
	if (_base > base) {
	    skip_len = 512 * (_base - base);
	    buf += skip_len;
	}
	read_len = 512 * (_limit - _base);
	oo = _ptr;
    }
    lk.unlock();

    if (read_len == 0)
	return std::make_pair(skip_len, read_len);

    extmap::obj_offset unit = {oo.obj, oo.offset / unit_sectors};
    sector_t blk_base = unit.offset * unit_sectors;
    sector_t blk_offset = oo.offset % unit_sectors;
    sector_t blk_top_offset = std::min({(int)(blk_offset+sectors),
		round_up(blk_offset+1,unit_sectors),
		(int)(blk_offset + (limit-base))});
    int n = -1;             // cache block number

    std::unique_lock lk2(m);
    bool in_cache = false;
    auto it2 = map.find(unit);
    if (it2 != map.end()) {
	n = it2->second;
	in_cache = true;
    }

    /* protection against random reads - read-around when hit rate is too low
     */
    bool use_cache = free_blks.size() > 0 && hit_stats.user * 3 > hit_stats.backend * 2;

    if (in_cache) {
	sector_t blk_in_ssd = super->base*8 + n*unit_sectors,
	    start = blk_in_ssd + blk_offset,
	    finish = start + (blk_top_offset - blk_offset);
	size_t bytes = 512 * (finish - start);

	a_bit[n] = true;
	hit_stats.user += bytes/512;

	if (buffer[n] != NULL) {
	    lk2.unlock();
	    memcpy(buf, buffer[n] + blk_offset*512, bytes);
	    cb(ptr);
	}
	else if (written[n]) {
	    auto closure = wrap([this, n, cb, ptr]{
		    cb(ptr);
		    in_use[n]--;
		    return true;
		});
	    in_use[n]++;
	    auto eio = new e_iocb;
	    e_io_prep_pread(eio, fd, buf, bytes, 512L*start, call_wrapped, closure);
	    e_io_submit(ioctx, eio);
	}
	else {              // prior read is pending
	    auto closure = wrap([this, n, buf, blk_offset, bytes, cb, ptr]{
		    memcpy(buf, buffer[n] + blk_offset*512, bytes);
		    cb(ptr);
		    return true;
		});
	    pending[n].push_back(closure);
	}
	read_len = bytes;
    }
    else if (use_cache) {
	/* assign a location in cache before we start reading (and while we're
	 * still holding the lock)
	 */
	map_dirty = true;
	n = free_blks.back();
	free_blks.pop_back();
	written[n] = false;
	in_use[n]++;
	map[unit] = n;
	flat_map[n] = unit;
	auto _buf = get_cacheline_buf(n);
	//printf("reading [%d] %p\n", n, _buf);

	hit_stats.backend += unit_sectors;
	sector_t sectors = blk_top_offset - blk_offset;
	hit_stats.user += sectors;
	lk2.unlock();

	off_t nvme_offset = (super->base*8 + n*unit_sectors) * 512L;
	off_t buf_offset = blk_offset * 512L;
	off_t bytes = 512L * sectors;

	auto write_done = wrap([this, n, _buf]{
		written[n] = true;
		return true;
	    });

	auto read_done =
	    wrap([this, n, write_done, nvme_offset, buf_offset, bytes, _buf,
		  buf, cb, ptr]{
		     std::unique_lock lk(m);
		     memcpy(buf, _buf + buf_offset, bytes);
		     buffer[n] = _buf;
		     //printf("setting buffer[%d] = %p\n", n, _buf);

		     std::vector<void*> v(std::make_move_iterator(pending[n].begin()),
					  std::make_move_iterator(pending[n].end()));
		     pending[n].erase(pending[n].begin(), pending[n].end());
		     lk.unlock();
		     cb(ptr);
		     for (auto p : v)
			 call_wrapped(p);
		     auto eio = new e_iocb;
		     e_io_prep_pwrite(eio, fd, _buf, unit_sectors*512L,
				      nvme_offset, call_wrapped, write_done);
		     e_io_submit(ioctx, eio);
		     return true;
		 });

	objname name(be->prefix(), unit.obj);
	auto cb_req = new callback_req(call_wrapped, read_done);
	iovec iov = {_buf, (size_t)(512L*unit_sectors)};
	auto req = io->make_read_req(name.c_str(), 512L*blk_base, &iov, 1);
	req->run(cb_req);

	read_len = bytes;
    }
    else {
	hit_stats.user += read_len / 512;
	hit_stats.backend += read_len / 512;
	lk2.unlock();

	objname name(be->prefix(), oo.obj);
	auto cb_req = new callback_req(cb, ptr);
	iovec iov = {buf, read_len};
	auto req = io->make_read_req(name.c_str(), 512L*oo.offset, &iov, 1);
	req->run(cb_req);

	// read_len unchanged
    }
    return std::make_pair(skip_len, read_len);
}

void read_cache_impl::write_map(void) {
    pwrite(fd, flat_map, 4096 * super->map_blocks, 4096L * super->map_start);
}

/* --------- Debug methods ----------- */

void read_cache_impl::get_info(j_read_super **p_super,
			       extmap::obj_offset **p_flat, 
			       std::vector<int> **p_free_blks,
			       std::map<extmap::obj_offset,int> **p_map) {
    if (p_super != NULL)
	*p_super = super;
    if (p_flat != NULL)
	*p_flat = flat_map;
    if (p_free_blks != NULL)
	*p_free_blks = &free_blks;
    if (p_map != NULL)
	*p_map = &map;
}

void read_cache_impl::do_add(extmap::obj_offset unit, char *buf) {
    std::unique_lock lk(m);
    char *_buf = (char*)aligned_alloc(512, 65536);
    memcpy(_buf, buf, 65536);
    int n = free_blks.back();
    free_blks.pop_back();
    written[n] = true;
    map[unit] = n;
    flat_map[n] = unit;
    off_t nvme_offset = (super->base*8 + n*unit_sectors)*512L;
    pwrite(fd, _buf, unit_sectors*512L, nvme_offset);
    write_map();
    free(_buf);
}

void read_cache_impl::do_evict(int n) {
    std::unique_lock lk(m);
    evict(n);
}

