/*
 * file:        write_cache.cc
 * description: write_cache implementation
 * 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <uuid/uuid.h>

#include <atomic>

#include <vector>
#include <map>
#include <stack>

#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <cassert>
#include <algorithm>

#include "lsvd_types.h"

#include "journal.h"
#include "smartiov.h"
#include "extent.h"
#include "misc_cache.h"
#include "backend.h"
#include "translate.h"
#include "io.h"
#include "request.h"
#include "nvme.h"

#include "write_cache.h"
#include "config.h"

extern void do_log(const char *, ...);

/* ------------- Write cache structure ------------- */
class write_cache_impl : public write_cache {
    size_t         dev_max;
    uint32_t       super_blkno;
    lsvd_config   *cfg;
    
    std::atomic<uint64_t> sequence = 1; // write sequence #

    /* bookkeeping for write throttle
     */
    int total_write_pages = 0;
    int max_write_pages = 0;
    int outstanding_writes = 0;
    size_t write_batch = 0;
    std::condition_variable write_cv;

    void evict(page_t base, page_t len);
    void send_writes(std::unique_lock<std::mutex> &lk);

    /* initialization stuff
     */
    void read_map_entries();
    int roll_log_forward();
    std::tuple<bool,page_t,uint64_t> chase(page_t, uint64_t, page_t);
    char *_hdrbuf;		// for reading at startup

    /* track contents of the write cache. 
     */
    enum page_type { WCACHE_NONE = 0,
		     WCACHE_HDR = 17,
		     WCACHE_PAD = 18,
		     WCACHE_DATA = 19};
    struct page_desc {
	enum page_type type = WCACHE_NONE;
	int            n_pages;
    };
    page_desc *cache_blocks;
    extmap::cachemap2 rmap;	// reverse map: pLBA,len -> vLBA

    /* track outstanding requests and point before which
     * all writes are durable in SSD
     */
    std::vector<std::pair<page_t,page_t>> outstanding;
    page_t next_acked_page = 0;
    void notify_complete(page_t start, page_t len);
    void record_outstanding(page_t start, page_t len);

    thread_pool<int>          *misc_threads;

    void flush_thread(thread_pool<int> *p);
    void write_checkpoint(void);
    
    /* allocate journal entry, create a header
     */
    uint32_t allocate(page_t n, page_t &pad, page_t &n_pad);
    std::vector<write_cache_work*> work;
    sector_t work_sectors;	// queued in work[]
    j_write_super *super;

    /* these are used by wcache_write_req
     */
    friend class wcache_write_req;
    std::mutex                m;
    extmap::cachemap2 map;
    translate        *be;
    j_hdr *mk_header(char *buf, uint32_t type, page_t blks);
    nvme 		      *nvme_w = NULL;

public:

    /* throttle writes with window of max_write_pages
     */
    void get_room(sector_t sectors); 
    void release_room(sector_t sectors);
    void flush(void);

    write_cache_impl(uint32_t blkno, int _fd, translate *_be,
		     lsvd_config *cfg);
    ~write_cache_impl();

    write_cache_work *writev(request *req, sector_t lba, smartiov *iov);
    virtual std::tuple<size_t,size_t,request*> 
        async_read(size_t offset, char *buf, size_t bytes);
    virtual std::tuple<size_t,size_t,request*> 
        async_readv(size_t offset, smartiov *iov);

    /* debug functions */

    /* getmap callback(ptr, base, limit, phys_lba)
     */
    void getmap(int base, int limit, int (*cb)(void*,int,int,int), void *ptr);

    void reset(void);
    void get_super(j_write_super *s); /* copies superblock */

    /*  @blk - header to read
     *  @extents - data to move. empty if J_PAD or J_CKPT
     *  return value - first page of next record
     */
    page_t get_oldest(page_t blk, std::vector<j_extent> &extents);
    void do_write_checkpoint(void);

    std::pair<std::mutex*,extmap::cachemap2*> getmap2(void) {
	return std::make_pair(&m, &map);
    }
};


/* ------------- batched write request ------------- */

class wcache_write_req : public request {
    std::atomic<int> reqs = 0;

    sector_t      plba;

    page_t        hdr_page;	// for tracking completions
    page_t        n_hdr_pages;
    page_t        pad_page = 0;
    page_t        n_pad_pages;
    
    std::vector<write_cache_work*> work;
    request      *r_data = NULL;
    char         *hdr = NULL;
    smartiov      *data_iovs;

    request      *r_pad = NULL;
    char         *pad_hdr = NULL;
    smartiov      pad_iov;

    write_cache_impl *wcache = NULL;
    
public:
    wcache_write_req(std::vector<write_cache_work*> *w, page_t n_pages,
		     page_t page, page_t n_pad, page_t pad,
		     write_cache *wcache);
    ~wcache_write_req();
    void run(request *parent);
    void notify(request *child);

    void wait() {}
    void release() {}		// TODO: free properly
};

/* n_pages: number of 4KB data pages (not counting header)
 * page:    page number to begin writing 
 * n_pad:   number of pages to skip (not counting header)
 * pad:     page number for pad entry (0 if none)
 */
wcache_write_req::wcache_write_req(std::vector<write_cache_work*> *work_,
				       page_t n_pages, page_t page,
				       page_t n_pad, page_t pad,
				       write_cache* wcache_)  {
    wcache = (write_cache_impl*)wcache_;
    for (auto w : *work_)
	work.push_back(w);
    
    if (pad != 0) {
	pad_hdr = (char*)aligned_alloc(512, 4096);
	wcache->mk_header(pad_hdr, LSVD_J_PAD, n_pad+1);

	pad_page = pad;	// track completion
	n_pad_pages = n_pad+1;
	wcache->record_outstanding(pad, n_pad+1);

	pad_iov.push_back((iovec){pad_hdr, 4096});
	reqs++;
	r_pad = wcache->nvme_w->make_write_request(&pad_iov, pad*4096L);
    }
  
    std::vector<j_extent> extents;
    for (auto w : work) {
	extents.push_back((j_extent){(uint64_t)w->lba, w->iov->bytes() / 512});
    }
  
    hdr = (char*)aligned_alloc(512, 4096);
    j_hdr *j = wcache->mk_header(hdr, LSVD_J_DATA, 1+n_pages);

    hdr_page = page;		// track completion
    n_hdr_pages = 1+n_pages;
    wcache->record_outstanding(page, 1+n_pages);

    j->extent_offset = sizeof(*j);
    size_t e_bytes = extents.size() * sizeof(j_extent);
    j->extent_len = e_bytes;
    memcpy((void*)(hdr + sizeof(*j)), (void*)extents.data(), e_bytes);

    plba = (page+1) * 8;

    data_iovs = new smartiov();
    data_iovs->push_back((iovec){hdr, 4096});
    for (auto w : work) {
	auto [iov, iovcnt] = w->iov->c_iov();
	data_iovs->ingest(iov, iovcnt);
    }
    reqs++;
    r_data = wcache->nvme_w->make_write_request(data_iovs, page*4096L);
}

wcache_write_req::~wcache_write_req() {
    free(hdr);
    if (pad_hdr) 
        free(pad_hdr);
    delete data_iovs;
    //delete work;
}

//extern void check_crc(sector_t sector, iovec *iov, int niovs, const char *msg);
std::mutex wc_m;
std::set<void*> wc_reqs;
static void request_start(wcache_write_req *r) {
    auto v = (void*)r;
    std::unique_lock lk(wc_m);
    wc_reqs.insert(v);
}
static void request_end(wcache_write_req *r) {
    auto v = (void*)r;
    std::unique_lock lk(wc_m);
    wc_reqs.erase(v);
}

void wcache_write_req::notify(request *child) {
    child->release();
    if(--reqs > 0)
	return;
    {
	std::unique_lock lk(wcache->m);
	auto _plba = plba;

	/* update the write cache forward and reverse maps
	 */
        std::vector<extmap::lba2lba> garbage; 
	for (auto w : work) {
	    sector_t sectors = w->iov->bytes() / 512;
	    wcache->map.update(w->lba, w->lba + sectors, _plba, NULL);
            wcache->rmap.update(_plba, _plba+sectors, w->lba);
	    _plba += sectors;
	}
        for (auto it = garbage.begin(); it != garbage.end(); it++) 
            wcache->rmap.trim(it->s.base, it->s.base+it->s.len);

	wcache->outstanding_writes--;
	if (wcache->work.size() >= wcache->write_batch ||
	    wcache->work_sectors >= wcache->cfg->wcache_chunk / 512)
	    wcache->send_writes(lk);

	if (pad_page != 0)
	    wcache->notify_complete(pad_page, n_pad_pages);
	wcache->notify_complete(hdr_page, n_hdr_pages);
    }

    /* send data to backend, invoke callbacks, then clean up
     */
    for (auto w : work) {
	auto [iov, iovcnt] = w->iov->c_iov();
	//check_crc(lba, iov, iovcnt, "3");
	wcache->be->writev(w->lba*512, iov, iovcnt);
    }
    for (auto w : work) {
	w->req->notify((request*)w);
	delete w;
    }

    /* we don't implement release or wait - just delete ourselves.
     */
    request_end(this);
    delete this;
}

void wcache_write_req::run(request *parent /* unused */) {
    if(r_pad) 
	r_pad->run(this);
    r_data->run(this);
}

/* --------------- Write Cache ------------- */

/* stall write requests using window of max_write_blocks, which should
 * be <= 0.5 * write cache size.
 */
void write_cache_impl::get_room(sector_t sectors) {
    int pages = sectors / 8;
    std::unique_lock lk(m);
    while (total_write_pages + pages > max_write_pages)
	write_cv.wait(lk);
    total_write_pages += pages;
}

void write_cache_impl::release_room(sector_t sectors) {
    int pages = sectors / 8;
    std::unique_lock lk(m);
    total_write_pages -= pages;
    if (total_write_pages < max_write_pages)
	write_cv.notify_all();
}

void write_cache_impl::flush(void) {
    std::unique_lock lk(m);
    while (total_write_pages > 0)
	write_cv.wait(lk);
}

/* call this BEFORE writing to cache, guarantees that we don't have any 
 * old map entries pointing to the locations being written to.
 */
void write_cache_impl::evict(page_t page, page_t limit) {
    //assert(!m.try_lock());  // m must be locked
    page_t b = super->base;
    page_t oldest = super->oldest;

    while (page < limit && cache_blocks[page - b].type == WCACHE_NONE)
	page++;
    if (page == limit)		// fresh cache - nothing to evict
	return;
    
    /* page < limit, so if we hit a pad we're done
     */
    assert(page == oldest);
    if (cache_blocks[page - b].type == WCACHE_PAD) {
	cache_blocks[page - b].type = WCACHE_NONE;
	page = oldest = super->oldest = super->base;
	limit = super->base + 10; // leave a bit of room
    }

    /* otherwise we're chipping away at the oldest journal records
     */
    assert(cache_blocks[oldest - b].type == WCACHE_HDR);

    while (oldest < limit) {
	page_t len = cache_blocks[oldest - b].n_pages;
	assert(oldest + len <= (page_t)super->limit);
	sector_t s_base = oldest*8, s_limit = s_base + len*8;
	
	for (auto it = rmap.lookup(s_base);
	     it != rmap.end() && it->base() < s_limit; it++) {
	    auto [_base, _limit, ptr] = it->vals(s_base, s_limit);
	    map.trim(ptr, ptr+(_limit-_base));
	}
	rmap.trim(s_base, s_limit);

	for (int i = 0; i < len; i++)
	    cache_blocks[oldest - b + i].type = WCACHE_NONE;

	oldest += len;

	assert(oldest <= (page_t)super->limit);
	if (oldest == (page_t)super->limit) {
	    oldest = super->base;
	    limit = oldest + 10;
	}
    }

    super->oldest = oldest;
}


/* must be called with lock held. 
 * n:        total length to allocate (including header)
 * <return>: page number for header
 * pad:      page number for pad header (or 0)
 * n_pad:    total pages for pad
 */
uint32_t write_cache_impl::allocate(page_t n, page_t &pad, page_t &n_pad) {
    //assert(!m.try_lock());
    pad = n_pad = 0;
    if (super->limit - super->next < n) {
	pad = super->next;
	n_pad = super->limit - pad;
	evict(pad, super->limit);
	super->next = super->base;
    }

    auto val = super->next;
    evict(val, std::min((val + n + 10), super->limit));

    super->next += n;
    if (super->next == super->limit)
	super->next = super->base;
    return val;
}

void write_cache_impl::notify_complete(page_t start, page_t len) {
    //assert(!m.try_lock());	// must be locked
    auto it = std::find(outstanding.begin(), outstanding.end(),
			std::make_pair(start,len));
    assert(it != outstanding.end());
    outstanding.erase(it);
    next_acked_page = outstanding.front().first;
}

void write_cache_impl::record_outstanding(page_t start, page_t len) {
    //assert(!m.try_lock());	// must be locked
    outstanding.push_back(std::make_pair(start, len));
}

/* call with lock held
 */
j_hdr *write_cache_impl::mk_header(char *buf, uint32_t type, page_t blks) {
    //assert(!m.try_lock());
    memset(buf, 0, 4096);
    j_hdr *h = (j_hdr*)buf;
    // OH NO - am I using wcache->sequence or wcache->super->seq???
    *h = (j_hdr){.magic = LSVD_MAGIC, .type = type, .version = 1,
		 .seq = sequence++, .len = blks, .crc32 = 0,
		 .extent_offset = 0, .extent_len = 0};
    return h;
}

void write_cache_impl::flush_thread(thread_pool<int> *p) {
    pthread_setname_np(pthread_self(), "wcache_flush");
    auto period = std::chrono::milliseconds(50);
    while (p->running) {
	std::unique_lock lk(m);
	//printf("%d %p\n", __LINE__, &m); fflush(stdout);
	p->cv.wait_for(lk, period);
	if (!p->running)
	    return;
	if (outstanding_writes == 0 && work.size() > 0)
	    send_writes(lk);
    }
}

/* note that this is only called on shutdown, so we don't
 * worry about locking, and we set the 'clean' flag in the superblock
 */
void write_cache_impl::write_checkpoint(void) {

    /* find all the journal record lengths in cache_blocks[.] first
     */
    std::vector<j_length> lengths;
    page_t b = super->base;
    for (int i = super->base; i < (int)super->limit; i++) {
	auto [type, n_pages] = cache_blocks[i - b];
	if (type == WCACHE_HDR && (i < (int)next_acked_page ||
				   i >= (int)super->oldest))
	    lengths.push_back((j_length){i, n_pages});
    }

    size_t map_bytes = map.size() * sizeof(j_map_extent);
    size_t len_bytes = lengths.size() * sizeof(j_length);
    page_t map_pages = div_round_up(map_bytes, 4096),
	len_pages = div_round_up(len_bytes, 4096),
	ckpt_pages = map_pages + len_pages;

    page_t blockno = super->meta_base;

    char *e_buf = (char*)aligned_alloc(512, 4096L*map_pages);
    auto jme = (j_map_extent*)e_buf;
    int n_extents = 0;
    if (map.size() > 0)
	for (auto it = map.begin(); it != map.end(); it++)
	    jme[n_extents++] = (j_map_extent){(uint64_t)it->s.base,
					      (uint64_t)it->s.len, (uint32_t)it->s.ptr};
    // valgrind:
    int pad1 = 4096L*map_pages - map_bytes;
    assert(map_bytes == n_extents * sizeof(j_map_extent));
    assert(pad1 + map_bytes == 4096UL*map_pages);
    if (pad1 > 0)
	memset(e_buf + map_bytes, 0, pad1);

    char *l_buf = (char*)aligned_alloc(512, 4096L * len_pages);
    auto jl = (j_length*)l_buf;
    int n_lens = 0;
    for (auto l : lengths)
	jl[n_lens++] = l;

    // valgrind:
    int pad2 = 4096L * len_pages - len_bytes;
    assert(len_bytes + pad2 == len_pages*4096UL);
    if (pad2 > 0)
	memset(l_buf + len_bytes, 0, pad2);

    /* shouldn't really need the copy, since it's only called on
     * shutdown, except that some unit tests call this and expect things
     * to work afterwards
     */
    j_write_super *super_copy = (j_write_super*)aligned_alloc(512, 4096);
    memcpy(super_copy, super, 4096);
    super_copy->seq = sequence;
    super_copy->next = next_acked_page;

    super_copy->map_start = super->map_start = blockno;
    super_copy->map_blocks = super->map_blocks = map_pages;
    super_copy->map_entries = super->map_entries = n_extents;

    super_copy->len_start = super->len_start = blockno+map_pages;
    super_copy->len_blocks = super->len_blocks = len_pages;
    super_copy->len_entries = super->len_entries = n_lens;

    super_copy->clean = true;

    assert(4096UL*blockno + 4096UL*ckpt_pages <= dev_max);
    iovec iov[] = {{e_buf, map_pages*4096UL},
		   {l_buf, len_pages*4096UL}};

    if (nvme_w->writev(iov, 2, 4096L*blockno) < 0)
	throw_fs_error("wckpt_e");
    if (nvme_w->write((char*)super_copy, 4096, 4096L*super_blkno) < 0)
	throw_fs_error("wckpt_s");

    free(super_copy);
    free(e_buf);
    free(l_buf);

}

void write_cache_impl::read_map_entries() {
    size_t map_bytes = super->map_entries * sizeof(j_map_extent),
	map_bytes_4k = round_up(map_bytes, 4096);
    char *map_buf = (char*)aligned_alloc(512, map_bytes_4k);
    std::vector<j_map_extent> extents;

    if (nvme_w->read(map_buf, map_bytes_4k, 4096L * super->map_start) < 0)
	throw_fs_error("wcache_map");
    decode_offset_len<j_map_extent>(map_buf, 0, map_bytes, extents);

    for (auto e : extents) {
	map.update(e.lba, e.lba+e.len, e.plba); // forward map
        rmap.update(e.plba, e.plba + e.len, e.lba); // reverse
    }
    free(map_buf);

    size_t len_bytes = super->len_entries * sizeof(j_length),
	len_bytes_4k = round_up(len_bytes, 4096);
    char *len_buf = (char*)aligned_alloc(512, len_bytes_4k);
    std::vector<j_length> _lengths;

    if (nvme_w->read(len_buf, len_bytes_4k, 4096L * super->len_start) < 0)
	throw_fs_error("wcache_len");
    decode_offset_len<j_length>(len_buf, 0, len_bytes, _lengths);

    auto b = super->base;
    for (auto l : _lengths) {
	cache_blocks[l.page - b] = (page_desc){WCACHE_HDR, l.len};
	for (int i = 1; i < l.len; i++)
	    cache_blocks[l.page - b + i].type = WCACHE_DATA;
    }

    free(len_buf);
}

/* returns:
 *   bool: false for failure
 *   page: next page after last record (super.limit if we hit end)
 *   seq: sequence number next for next record
 */
std::tuple<bool,page_t,uint64_t> write_cache_impl::chase(page_t page,
							 uint64_t seq,
							 page_t limit) {
    auto hdr = (j_hdr*)_hdrbuf;

    if (nvme_w->read(_hdrbuf, 4096, 4096L * page) < 0)
	throw_fs_error("cache log roll-forward");
    while (hdr->magic == LSVD_MAGIC && hdr->seq == seq && page < limit) {
	page += hdr->len;
	seq++;
	if (nvme_w->read(_hdrbuf, 4096, 4096L * page) < 0)
	    throw_fs_error("cache log roll-forward");
    }
    if (page < limit && hdr->magic == LSVD_MAGIC)
	return std::make_tuple(false, 0, 0);

    return std::make_tuple(true, page, seq);
}

int write_cache_impl::roll_log_forward() {
    auto hdr = (j_hdr*)_hdrbuf;
    page_t b = super->base;

    /* locate the ranges of valid records - from part1_min to part1_max
     * and optionally part2_min to part2_max (if we wrapped around)
     * note that 0 is an invalid page number
     */
    std::pair<page_t,uint64_t> part1_min(0,0), part1_max(0,0),
	part2_min(0,0), part2_max(0,0);

    if (nvme_w->read(_hdrbuf, 4096, 4096L * b) < 0)
	throw_fs_error("cache log roll-forward");

    /* since we're not checking CRCs, there will always be at least
     * one valid record at the beginning of the log
     * find the end of that run, set part1_min/max
     */
    part1_min = std::make_pair(b, hdr->seq);
    auto [rv, _page, _seq] = chase(b, hdr->seq, super->limit);
    if (!rv)
	return -1;

    /* find a run starting after part1_max. If we find one, it must
     * be earlier, so part2 <- part1, part1 = the run we found
     */
    part1_max = std::make_pair(_page, _seq);
    for (b = _page; b < (page_t)super->limit; b++) {
	if (nvme_w->read(_hdrbuf, 4096, 4096L * b) < 0)
	    throw_fs_error("cache log roll-forward");

	if (hdr->magic != LSVD_MAGIC ||
	    (hdr->type != LSVD_J_DATA && hdr->type != LSVD_J_PAD) ||
	    (hdr->seq > part1_min.second))
	    continue;
	    
	auto _seq0 = hdr->seq;
	auto [rv, _page, _seq] = chase(b, hdr->seq, super->limit);
	if (rv && _page == (page_t)super->limit) {
	    part2_min = part1_min;
	    part2_max = part1_max;
	    part1_min = std::make_pair(b, _seq0);
	    part1_max = std::make_pair(_page, _seq);
	    break;
	}
    }

    /* Now read all the records in, update lengths and map,
     * and write to backend.
     */
    sequence = part1_min.second;
    page_t start = super->oldest = part1_min.first;
    page_t end = super->next = part2_max.first;
    if (end == 0)
	end = part1_max.first;
    
    for (b = start; b != end; ) {
	if (nvme_w->read(_hdrbuf, 4096, 4096L * b) < 0)
	    throw_fs_error("wcache");

	/* we've already verified everything, so this shouldn't fail, 
	 * right???
	 */
	if (hdr->magic != LSVD_MAGIC ||
	    (hdr->type != LSVD_J_DATA && hdr->type != LSVD_J_PAD) ||
	    hdr->seq != sequence)
	    return -1;
	
	sequence++;
	page_t idx = b - super->base;

	/* update the record length accounting
	 */
	if (hdr->type == LSVD_J_PAD) {
	    cache_blocks[idx++] =
		(page_desc){WCACHE_PAD, (int)(super->limit - super->next)};
	    while (idx < (page_t)(super->limit - super->base))
		cache_blocks[idx++].type = WCACHE_NONE;
	    b = super->base;
	    continue;
	}

	cache_blocks[idx] = (page_desc){WCACHE_HDR, (int)hdr->len};
	for (int i = 1; i < (int)hdr->len; i++)
	    cache_blocks[idx+i].type = WCACHE_DATA;

	/* read LBA info from header, read data, then 
	 * - put mappings into cache map and rmap
	 * - write data to backend
	 */
	std::vector<j_extent> entries;
	decode_offset_len<j_extent>(_hdrbuf, hdr->extent_offset,
				    hdr->extent_len, entries);

	size_t data_len = 4096L * (hdr->len - 1);
	char *data = (char*)aligned_alloc(512, data_len);
	if (nvme_w->read(data, data_len, 4096L * (super->next + 1)) < 0)
	    throw_fs_error("wcache");

	sector_t plba = (b+1) * 8;
	size_t offset = 0;
	std::vector<extmap::lba2lba> garbage;
	
	for (auto e : entries) {
	    map.update(e.lba, e.lba+e.len, plba, &garbage);
	    rmap.update(plba, plba+e.len, e.lba);

	    size_t bytes = e.len * 512;
	    iovec iov = {data+offset, bytes};
	    be->writev(e.lba*512, &iov, 1);
	    offset += bytes;
	    plba += e.len;
	}
	for (auto g : garbage)
	    rmap.trim(g.s.base, g.s.base+g.s.len);	    

	free(data);
	b += hdr->len;
	if (b >= (page_t)super->limit)
	    b = super->base;
    }

    return 0;
}

write_cache_impl::write_cache_impl( uint32_t blkno, int fd, translate *_be,
				    lsvd_config *cfg_) {
    super_blkno = blkno;
    dev_max = getsize64(fd);
    be = _be;
    cfg = cfg_;

    _hdrbuf = (char*)aligned_alloc(512, 4096);
    
    const char *name = "write_cache_cb";
    nvme_w = make_nvme(fd, name);

    char *buf = (char*)aligned_alloc(512, 4096);
    if (nvme_w->read(buf, 4096, 4096L*super_blkno) < 4096)
	throw_fs_error("wcache");
    
    super = (j_write_super*)buf;
    int n_pages = super->limit - super->base;
    cache_blocks = new page_desc[n_pages];

    /* if it's clean we can read in the map and lengths, otherwise
     * do crash recovery. Then set the dirty flag
     */
    if (super->clean) {
	sequence = super->seq;
	read_map_entries();	// length, too
    }
    else
	roll_log_forward();
    super->clean = false;
    if (nvme_w->write(buf, 4096, 4096L*super_blkno) < 4096)
	throw_fs_error("wcache");
    
    max_write_pages = n_pages / 2;
    write_batch = cfg->wcache_batch;
    
    misc_threads = new thread_pool<int>(&m);
    misc_threads->pool.push(std::thread(&write_cache_impl::flush_thread,
					this, misc_threads));
}

write_cache *make_write_cache(uint32_t blkno, int fd,
			      translate *be, lsvd_config *cfg) {
    return new write_cache_impl(blkno, fd, be, cfg);
}

write_cache_impl::~write_cache_impl() {
    delete misc_threads;
    delete[] cache_blocks;
    free(super);
    free(_hdrbuf);
    delete nvme_w;
}

void write_cache_impl::send_writes(std::unique_lock<std::mutex> &lk) {
    sector_t sectors = 0;
    for (auto w : work) {
        sectors += w->iov->bytes() / 512;
        assert(w->iov->aligned(512));
    }
    
    page_t pages = div_round_up(sectors, 8);
    page_t pad, n_pad;
    page_t page = allocate(pages+1, pad, n_pad);
    auto b = super->base;
    
    if (pad) {
	page_t pad_len = super->limit - pad;
	evict(pad, super->limit);
	cache_blocks[pad - b] = (page_desc){WCACHE_PAD, pad_len};
	for (page_t i = pad+1; i < (page_t)super->limit; i++)
	    cache_blocks[i - b].type = WCACHE_NONE;
    }

    cache_blocks[page - b] = (page_desc){WCACHE_HDR, pages+1};
    for (int i = 0; i < pages; i++)
	cache_blocks[page - b + 1 + i].type = WCACHE_DATA;
    

    auto req = new wcache_write_req(&work, pages, page, n_pad-1, pad, this);
    request_start(req);
    work.erase(work.begin(), work.end());
    work_sectors = 0;
    outstanding_writes++;
    
    //lk.unlock();
    req->run(NULL);
    //lk.lock();
}

write_cache_work *write_cache_impl::writev(request *req, sector_t lba, smartiov *iov) {
    std::unique_lock lk(m);
    //printf("%d %p\n", __LINE__, &m); fflush(stdout);
    auto w = new write_cache_work(req, lba, iov);
    work.push_back(w);
    work_sectors += iov->bytes() / 512L;

    // if we're not under write pressure, send writes immediately; else
    // batch them
    if (outstanding_writes == 0 || work.size() >= write_batch ||
	work_sectors >= cfg->wcache_chunk / 512)
	send_writes(lk);
    return w;
}

/* arguments:
 *  lba to start at
 *  iov corresponding to lba (iov.bytes() = length to read)
 * returns (number of bytes skipped), (number of bytes read_started)
 */
std::tuple<size_t,size_t,request*>
write_cache_impl::async_read(size_t offset, char *buf, size_t bytes) {
    sector_t base = offset/512, limit = base + bytes/512;
    size_t skip_len = 0, read_len = 0;
    request *rreq = NULL;
    
    std::unique_lock<std::mutex> lk(m);
    off_t nvme_offset = 0;

    auto it = map.lookup(base);
    if (it == map.end() || it->base() >= limit)
	skip_len = bytes;
    else {
	auto [_base, _limit, plba] = it->vals(base, limit);
	if (_base > base) {
	    skip_len = 512 * (_base - base);
	    buf += skip_len;
	}
	read_len = 512 * (_limit - _base);
	nvme_offset = 512L * plba;
    }
    lk.unlock();

    if (read_len) 
	rreq = nvme_w->make_read_request(buf, read_len, nvme_offset);

    return std::make_tuple(skip_len, read_len, rreq);
}

std::tuple<size_t,size_t,request*>
write_cache_impl::async_readv(size_t offset, smartiov *iov) {
    auto bytes = iov->bytes();
    sector_t base = offset/512, limit = base + bytes/512;
    size_t skip_len = 0, read_len = 0;
    request *rreq = NULL;
    
    std::unique_lock<std::mutex> lk(m);
    off_t nvme_offset = 0;

    auto it = map.lookup(base);
    if (it == map.end() || it->base() >= limit)
	skip_len = bytes;
    else {
	auto [_base, _limit, plba] = it->vals(base, limit);
	if (_base > base) 
	    skip_len = 512 * (_base - base);
	read_len = 512 * (_limit - _base);
	nvme_offset = 512L * plba;
    }

    if (read_len) {
	smartiov _iovs = iov->slice(skip_len, skip_len+read_len);
	rreq = nvme_w->make_read_request(&_iovs, nvme_offset);
    }

    return std::make_tuple(skip_len, read_len, rreq);
}

// debugging
void write_cache_impl::getmap(int base, int limit, int (*cb)(void*, int, int, int),
			 void *ptr) {
    for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
	auto [_base, _limit, plba] = it->vals(base, limit);
	if (!cb(ptr, (int)_base, (int)_limit, (int)plba))
	    break;
    }
}

void write_cache_impl::reset(void) {
    map.reset();
}

void write_cache_impl::get_super(j_write_super *s) {
    *s = *super;
}

/* debug:
 * cache eviction - get info from oldest entry in cache. [should be private]
 *  @blk - header to read
 *  @extents - data to move. empty if J_PAD or J_CKPT
 *  return value - first page of next record
 */

page_t write_cache_impl::get_oldest(page_t blk, std::vector<j_extent> &extents) {
    char *buf = (char*)aligned_alloc(512, 4096);
    j_hdr *h = (j_hdr*)buf;

    if (nvme_w->read(buf, 4096, blk*4096) < 0)
	throw_fs_error("wcache");
    if (h->magic != LSVD_MAGIC) {
	printf("bad block: %d\n", blk);
    }
    assert(h->magic == LSVD_MAGIC && h->version == 1);

    auto next_blk = blk + h->len;
    if (next_blk >= super->limit)
	next_blk = super->base;
    if (h->type == LSVD_J_DATA)
	decode_offset_len<j_extent>(buf, h->extent_offset, h->extent_len, extents);

    free(buf);
    return next_blk;
}
    
void write_cache_impl::do_write_checkpoint(void) {
    write_checkpoint();
}

