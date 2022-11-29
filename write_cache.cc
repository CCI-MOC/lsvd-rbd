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

class wcache_write_req;
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
    void send_writes(void);

    /* initialization stuff
     */
    void read_map_entries();
    int roll_log_forward();
    char *_hdrbuf;		// for reading at startup

    /* 
     * track contents of the write cache. 
     *    before:         after:     
     * +---+--+-----+ +--+----+---+ +-----+
     * |   |  |     | |  |    |   | |empty|
     * +---+--+-----+ +--+----+---+ +-----+
     *               ^
     *    allocation point (super->next)
     * corner cases are when one or more lists are empty, e.g. if
     * next==base, before=[]
     */
    std::vector<j_length> before; // writes before super->next
    std::vector<j_length> after;  // writes after super->next
    
    extmap::cachemap2 rmap;	// reverse map: pLBA,len -> vLBA
    friend void print_pages(write_cache_impl*, const char*);
    
    /* track outstanding requests and point before which
     * all writes are durable in SSD. Pad
     */
    struct write_record {
	uint64_t          seq;
	wcache_write_req *req = NULL;
	page_t            next = 0; // next acked page
	mutable bool      done = false;
	bool operator<(const write_record &other) const {
	    return seq < other.seq;
	}
    };
    std::set<write_record> outstanding;
    page_t next_acked_page = 0;
    void notify_complete(uint64_t seq, std::unique_lock<std::mutex> &lk);
    void record_outstanding(uint64_t seq, page_t next, wcache_write_req *req);

    thread_pool<int>          *misc_threads;

    void flush_thread(thread_pool<int> *p);
    void write_checkpoint(void);
    
    /* allocate journal entry, create a header
     */
    uint32_t allocate(page_t n, page_t &pad, page_t &n_pad, page_t &prev);
    std::vector<write_cache_work*> work;
    sector_t work_sectors;	// queued in work[]
    j_write_super *super;
    page_t         previous_hdr = 0;

    /* these are used by wcache_write_req
     */
    friend class wcache_write_req;
    std::mutex                m;
    extmap::cachemap2 map;
    translate        *be;
    j_hdr *mk_header(char *buf, uint32_t type, page_t blks, page_t prev);
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

void print_pages(write_cache_impl *w, const char *file) {
    FILE *fp = fopen(file, "w");
    fprintf(fp, "before:\n");
    for (auto l : w->before)
	fprintf(fp, " %d+%d\n", l.page, l.len);
    fprintf(fp, "after:\n");
    for (auto l : w->after)
	fprintf(fp, " %d+%d\n", l.page, l.len);
    fclose(fp);
}



/* ------------- batched write request ------------- */

class wcache_write_req : public request {
    std::atomic<int> reqs = 0;

    sector_t      plba;
    uint64_t      seq;
    
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
		     page_t page, page_t n_pad, page_t pad, page_t prev,
		     write_cache *wcache);
    ~wcache_write_req();
    void run(request *parent);
    void notify(request *child);
    void notify_in_order(std::unique_lock<std::mutex> &lk);
    
    void wait() {}
    void release() {}		// TODO: free properly
};

static char *z_page;
void pad_out(smartiov &iov, int pages) {
    if (z_page == NULL) {
	z_page = (char*)aligned_alloc(512, 4096);
	memset(z_page, 0, 4096); // make valgrind happy
    }
    for (int i = 0; i < pages; i++)
	iov.push_back((iovec){z_page, 4096});
}

/* n_pages: number of 4KB data pages (not counting header)
 * page:    page number to begin writing 
 * n_pad:   number of pages to skip (not counting header)
 * pad:     page number for pad entry (0 if none)
 */
wcache_write_req::wcache_write_req(std::vector<write_cache_work*> *work_,
				   page_t n_pages, page_t page,
				   page_t n_pad, page_t pad,
				   page_t prev, write_cache* wcache_)  {
    wcache = (write_cache_impl*)wcache_;
    for (auto w : *work_)
	work.push_back(w);

    /* if we pad, then the data record has prev=pad, and the pad 
     * has prev=prev
     */
    if (pad != 0) {
	pad_hdr = (char*)aligned_alloc(512, 4096);
	wcache->mk_header(pad_hdr, LSVD_J_PAD, n_pad+1, prev);
	prev = pad;
	
	/* if we pad before journal wraparound, zero out the remaining
	 * pages to make crash recovery easier.
	 */
	pad_iov.push_back((iovec){pad_hdr, 4096});
	pad_out(pad_iov, n_pad);
	reqs++;
	r_pad = wcache->nvme_w->make_write_request(&pad_iov, pad*4096L);
    }
  
    std::vector<j_extent> extents;
    for (auto w : work) {
	extents.push_back((j_extent){(uint64_t)w->lba, w->iov->bytes() / 512});
    }

    /* TODO: don't assign seq# in mk_header
     */
    hdr = (char*)aligned_alloc(512, 4096);
    j_hdr *j = wcache->mk_header(hdr, LSVD_J_DATA, 1+n_pages, prev);
    seq = j->seq;
    
    /* track completion 
     */
    wcache->record_outstanding(seq, page+n_pages+1, this);

    j->extent_offset = sizeof(*j);
    size_t e_bytes = extents.size() * sizeof(j_extent);
    j->extent_len = e_bytes;
    memcpy((void*)(hdr + sizeof(*j)), (void*)extents.data(), e_bytes);

    plba = (page+1) * 8;
    int i = 0;
    data_iovs = new smartiov();
    data_iovs->push_back((iovec){hdr, 4096});
    for (auto w : work) {
	auto [iov, iovcnt] = w->iov->c_iov();
	data_iovs->ingest(iov, iovcnt);
	i += w->iov->bytes() / 512;
    }
    reqs++;
    r_data = wcache->nvme_w->make_write_request(data_iovs, page*4096L);
}

wcache_write_req::~wcache_write_req() {
    free(hdr);
    if (pad_hdr) 
        free(pad_hdr);
    delete data_iovs;
}

/* called in order from notify_complete
 * write cache lock must be held
 */
void wcache_write_req::notify_in_order(std::unique_lock<std::mutex> &lk) {
    auto _plba = plba;

    /* update the write cache forward and reverse maps
     */
    std::vector<extmap::lba2lba> garbage; 
    for (auto w : work) {
	sector_t sectors = w->iov->bytes() / 512;
	wcache->map.update(w->lba, w->lba + sectors, _plba, &garbage);
	wcache->rmap.update(_plba, _plba+sectors, w->lba);
	_plba += sectors;
    }
    /* remove old mappings from the reverse map
     */
    for (auto it = garbage.begin(); it != garbage.end(); it++) 
	wcache->rmap.trim(it->s.ptr, it->s.ptr+it->s.len);

    /* if there are enough pending writes, send them
     */
    wcache->outstanding_writes--;
    if (wcache->work.size() >= wcache->write_batch ||
	wcache->work_sectors >= wcache->cfg->wcache_chunk / 512)
	wcache->send_writes();

    /* send data to backend, invoke callbacks, then clean up
     */
    for (auto w : work) {
	auto [iov, iovcnt] = w->iov->c_iov();
	//check_crc(lba, iov, iovcnt, "3");
	wcache->be->writev(seq, w->lba*512, iov, iovcnt);
    }
    lk.unlock();
    for (auto w : work) {
	w->req->notify((request*)w);
	delete w;
    }
    lk.lock();
    
    /* we don't implement release or wait - just delete ourselves.
     */
    delete this;
}

void wcache_write_req::notify(request *child) {
    child->release();
    if(--reqs > 0)
	return;

    std::unique_lock lk(wcache->m);
    wcache->notify_complete(seq, lk);
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
    while (total_write_pages > 0 || outstanding.size() > 0)
	write_cv.wait(lk);
}

/* called from allocate - dump map entries for locations being
 * reallocated.
 */
void write_cache_impl::evict(page_t p_base, page_t p_limit) {
    sector_t s_base = p_base*8, s_limit = p_limit*8;
    for (auto it = rmap.lookup(s_base);
	 it != rmap.end() && it->base() < s_limit; it++) {
	auto [_base, _limit, ptr] = it->vals(s_base, s_limit);
	map.trim(ptr, ptr+(_limit-_base));
    }
    rmap.trim(s_base, s_limit);
}

/* must be called with lock held. 
 * n:        total length to allocate (including header)
 * <return>: page number for header
 * pad:      page number for pad header (or 0)
 * n_pad:    total pages for pad
 * prev_:    previously
 *
 * TODO: totally confusing that n_pad includes the header page here,
 * while it excluses the header page in wcache_write_req constructor
 */
uint32_t write_cache_impl::allocate(page_t n, page_t &pad, page_t &n_pad,
				    page_t &prev) {
    //assert(!m.try_lock());
    assert(n > 0);
    assert(super->next >= super->base && super->next < super->limit);

    prev = previous_hdr;
    pad = n_pad = 0;
    page_t start = super->next, end = start;

again:
    while ((end - start) < n && after.size() > 0) {
	end = after.front().page;
	if (end < start + n) {
	    auto it = after.begin();
	    evict(it->page, it->page + it->len);
	    end = it->page + it->len;
	    after.erase(it);
	}
    }

    if (after.size() == 0)
	end = super->limit;

    if (end - start < n) {
	assert(after.size() == 0); // looping twice not allowed
	assert(pad == 0);	   // ditto
	pad = start;
	n_pad = end - start;
	after.insert(after.end(), before.begin(), before.end());
	before.clear();
	start = end = super->base;
	goto again;
    }

    before.push_back({start, n});
    super->next = start + n;
    if (super->next == super->limit) {
	super->next = super->base;
	after.insert(after.end(), before.begin(), before.end());
	before.clear();
    }

    previous_hdr = start;
    return start;
}

void write_cache_impl::notify_complete(uint64_t seq,
				       std::unique_lock<std::mutex> &lk) {
    //assert(!m.try_lock());	// must be locked
    write_record r = {seq, NULL, 0, false};
    auto it = outstanding.find(r);
    assert(it != outstanding.end() && it->seq == seq);
    it->done = true;

    std::vector<wcache_write_req*> reqs;
    for (it = outstanding.begin(); it != outstanding.end() && it->done;) {
	next_acked_page = it->next;
	reqs.push_back(it->req);
	it = outstanding.erase(it);
    }

    for (auto r : reqs)
	r->notify_in_order(lk);
    write_cv.notify_all();
}

void write_cache_impl::record_outstanding(uint64_t seq, page_t next,
					  wcache_write_req *req) {
    //assert(!m.try_lock());	// must be locked
    write_record r = {seq, req, next, false};
    outstanding.insert(r);
}

/* call with lock held
 */
j_hdr *write_cache_impl::mk_header(char *buf, uint32_t type, page_t blks,
				   page_t prev) {
    //assert(!m.try_lock());
    memset(buf, 0, 4096);
    j_hdr *h = (j_hdr*)buf;
    // OH NO - am I using wcache->sequence or wcache->super->seq???
    *h = (j_hdr){.magic = LSVD_MAGIC, .type = type, .version = 1,
		 .len = blks, .seq = sequence++, .crc32 = 0,
		 .extent_offset = 0, .extent_len = 0, .prev = prev};
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
	    send_writes();
    }
}

/* note that this is only called on shutdown, so we don't
 * worry about locking, and we set the 'clean' flag in the superblock
 */
void write_cache_impl::write_checkpoint(void) {

    /* get all the lengths
     */
    std::vector<j_length> lengths;
    lengths.insert(lengths.end(), before.begin(), before.end());
    lengths.insert(lengths.end(), after.begin(), after.end());

    /* and the map
     */
    size_t map_bytes = map.size() * sizeof(j_map_extent);
    size_t len_bytes = lengths.size() * sizeof(j_length);
    page_t map_pages = div_round_up(map_bytes, 4096),
	len_pages = div_round_up(len_bytes, 4096),
	ckpt_pages = map_pages + len_pages;

    char *e_buf = (char*)aligned_alloc(512, 4096L*map_pages);
    auto jme = (j_map_extent*)e_buf;
    int n_extents = 0;
    if (map.size() > 0)
	for (auto it = map.begin(); it != map.end(); it++)
	    jme[n_extents++] = (j_map_extent){(uint64_t)it->s.base,
					      (uint64_t)it->s.len, (uint32_t)it->s.ptr};

    // valgrind: clean up the end
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
    page_t blockno = super->meta_base;
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
    /* 
     * first read the checkpoint map and update map,rmap
     */
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

    /* read the lengths
     */
    size_t len_bytes = super->len_entries * sizeof(j_length),
	len_bytes_4k = round_up(len_bytes, 4096);
    char *len_buf = (char*)aligned_alloc(512, len_bytes_4k);
    std::vector<j_length> lengths;

    if (nvme_w->read(len_buf, len_bytes_4k, 4096L * super->len_start) < 0)
	throw_fs_error("wcache_len");
    decode_offset_len<j_length>(len_buf, 0, len_bytes, lengths);
    free(len_buf);
    std::sort(lengths.begin(), lengths.end());
    
    int max_page = 0;
    for (auto l : lengths) {
	page_t limit = l.page + l.len;
	if (limit <= super->next)
	    before.push_back(l);
	else
	    after.push_back(l);
	if (max_page < limit)
	    max_page = limit;
    }

    // (super->next == base) ==> (before == [])
    assert(super->next != super->base || before.size() == 0);
    // (before != []) ==> before <= super->next
    assert(before.size() == 0 ||
	     before.back().page + before.back().len <= super->next);
    
    next_acked_page = super->next;
}

/* needs to set the following variables:
 *  super->next
 *  next_acked_page
 *  sequence
 */
int write_cache_impl::roll_log_forward() {
    page_t start = super->base, prev = 0;
    auto h = (j_hdr*)_hdrbuf;
    
    if (nvme_w->read(_hdrbuf, 4096, 4096L * start) < 0)
	throw_fs_error("cache log roll-forward");

    /* nothing in cache
     */
    if (h->magic != LSVD_MAGIC || h->type != LSVD_J_DATA) {
	sequence = 1;
	super->next = next_acked_page = super->base;
	// before = after = {}
	return 0;
    }
    sequence = h->seq;

    /* find the oldest journal entry
     */
    while (true) {
	prev = h->prev;
	if (prev == 0)		// hasn't wrapped
	    break;
	if (nvme_w->read(_hdrbuf, 4096, 4096L * prev) < 0)
	    throw_fs_error("cache log roll-forward");
	if (h->magic != LSVD_MAGIC || h->seq != sequence-1 ||
	    (h->type != LSVD_J_DATA && h->type != LSVD_J_PAD))
	    break;
	sequence = h->seq;
	start = prev;
    }

    /* Read all the records in, update lengths and map, and write
     * to backend if they're newer than the last write the translation
     * layer guarantees is persisted.
     */
    while (true) {
	/* handle wrap-around
	 */
	if (start == super->limit) {
	    after.insert(after.end(), before.begin(), before.end());
	    before.clear();
	    start = super->base;
	    continue;
	}

	/* is this another journal record?
	 */
	if (nvme_w->read(_hdrbuf, 4096, 4096L * start) < 0)
	    throw_fs_error("cache log roll-forward");
	if (h->magic != LSVD_MAGIC || h->seq != sequence ||
	    (h->type != LSVD_J_DATA && h->type != LSVD_J_PAD))
	    break;

	if (h->type == LSVD_J_PAD) {
	    start = super->limit;
	    sequence++;
	    continue;
	}
	
	before.push_back({start, h->len});

	/* read LBA info from header, read data, then 
	 * - put mappings into cache map and rmap
	 * - write data to backend
	 */
	std::vector<j_extent> entries;
	decode_offset_len<j_extent>(_hdrbuf, h->extent_offset,
				    h->extent_len, entries);

	size_t data_len = 4096L * (h->len - 1);
	char *data = (char*)aligned_alloc(512, data_len);
	if (nvme_w->read(data, data_len, 4096L * (start+1)) < 0)
	    throw_fs_error("wcache");

	sector_t plba = (start+1) * 8;
	size_t offset = 0;
	std::vector<extmap::lba2lba> garbage;

	/* all write batches with sequence < max_cache_seq are
	 * guaranteed to be persisted in the backend already
	 */
	for (auto e : entries) {
	    map.update(e.lba, e.lba+e.len, plba, &garbage);
	    rmap.update(plba, plba+e.len, e.lba);

	    size_t bytes = e.len * 512;
	    iovec iov = {data+offset, bytes};
	    if (sequence >= be->max_cache_seq) {
		do_log("write %ld %d+%d\n", sequence.load(), (int)e.lba, e.len);
		be->writev(sequence, e.lba*512, &iov, 1);
	    }
	    else
		do_log("skip %ld %d (max %d) %ld+%d\n", start,
		       sequence.load(), be->max_cache_seq, e.lba, e.len);
	    offset += bytes;
	    plba += e.len;
	}
	for (auto g : garbage)
	    rmap.trim(g.s.base, g.s.base+g.s.len);	    

	free(data);

	start += h->len;
	sequence++;
    }

    super->next = next_acked_page = start;
    
    be->flush();
    usleep(10000);
    
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

    /* if it's clean we can read in the map and lengths, otherwise
     * do crash recovery. Then set the dirty flag
     */
    if (super->clean) {
	sequence = super->seq;
	read_map_entries();	// length, too
    }
    else {
	auto rv = roll_log_forward();
	assert(rv == 0);
    }
    super->clean = false;
    if (nvme_w->write(buf, 4096, 4096L*super_blkno) < 4096)
	throw_fs_error("wcache");
    
    int n_pages = super->limit - super->base;
    max_write_pages = n_pages / 2 + n_pages / 4;
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
    free(super);
    free(_hdrbuf);
    delete nvme_w;
}

void write_cache_impl::send_writes(void) {
    sector_t sectors = 0;
    for (auto w : work) {
        sectors += w->iov->bytes() / 512;
        assert(w->iov->aligned(512));
    }
    
    page_t pages = div_round_up(sectors, 8);
    page_t pad, n_pad, prev = 0;
    page_t page = allocate(pages+1, pad, n_pad, prev);

    auto req = new wcache_write_req(&work, pages, page,
				    n_pad-1, pad, prev, this);
    work.clear();
    work_sectors = 0;
    outstanding_writes++;
    
    req->run(NULL);
}

write_cache_work *write_cache_impl::writev(request *req, sector_t lba, smartiov *iov) {
    std::unique_lock lk(m);
    //do_log("wc %d+%d %d\n", lba, iov->bytes()/512, ((int*)(iov->data()->iov_base))[1]);
    auto w = new write_cache_work(req, lba, iov);
    work.push_back(w);
    work_sectors += iov->bytes() / 512L;

    // if we're not under write pressure, send writes immediately; else
    // batch them
    if (outstanding_writes == 0 || work.size() >= write_batch ||
	work_sectors >= cfg->wcache_chunk / 512)
	send_writes();
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

