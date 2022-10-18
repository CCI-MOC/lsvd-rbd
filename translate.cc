/*
 * file:        translate.cc
 * description: core translation layer - implementation
 * 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <unistd.h>
#include <sys/uio.h>
#include <string.h>

#include <uuid/uuid.h>

#include <vector>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <atomic>

#include <stack>
#include <map>

#include <algorithm>

#include <thread>

#include "extent.h"
#include "lsvd_types.h"
#include "request.h"
#include "objects.h"
#include "objname.h"
#include "config.h"
#include "translate.h"

#include "backend.h"
#include "smartiov.h"
#include "misc_cache.h"


void do_log(const char*, ...);
void fp_log(const char*, ...);

/* ----------- Object translation layer -------------- */

class translate_impl : public translate {
    /* lock ordering: lock m before *map_lock
     */
    std::mutex         m;	// for things in this instance
    extmap::objmap    *map;	// shared object map
    std::shared_mutex *map_lock; // locks the object map
    lsvd_config       *cfg;

    std::atomic<int> seq;

    friend class translate_req;
    
    class batch {
    public:
	char  *buf = NULL;	// data goes here
	size_t len = 0;		// current data size
	size_t max;		// done when len hits here
	std::vector<data_map> entries;
	int    seq = 0;		// sequence number for backend

	batch(size_t bytes){
	    buf = (char*)malloc(bytes);
	    max = bytes;
	}
	~batch(){
	    free(buf);
	}
	void append(uint64_t lba, smartiov *iov) {
	    auto bytes = iov->bytes();
	    entries.push_back((data_map){lba, bytes/512});
	    char *ptr = buf + len;
	    iov->copy_out(ptr);
	    len += bytes;
	}
    };
    batch *b = NULL;
    
    /* info on all live objects - all sizes in sectors */
    struct obj_info {
	int hdr;		// sectors
	int data;		// sectors
	int live;		// sectors
	enum obj_type type;	// LSVD_DATA or LSVD_CKPT
    };
    std::map<int,obj_info> object_info;

    std::queue<uint32_t> checkpoints;
    
    /* tracking completions for flush()
     */
    int       next_compln = 1;
    int       last_ckpt = 0;	// special value - see make_data_hdr
    int       last_sent = 0;	// most recent data object
    
    std::vector<bool> done;
    std::condition_variable cv;

    /* various constant state
     */
    char      single_prefix[128];

    /* superblock has two sections: [obj_hdr] [super_hdr]
     */
    char      *super_name;
    char      *super_buf = NULL;
    obj_hdr   *super_h = NULL;
    super_hdr *super_sh = NULL;
    size_t     super_len;

    thread_pool<batch*> workers;
    thread_pool<int>    misc_threads;

    /* for triggering GC
     */
    sector_t total_sectors = 0;
    sector_t total_live_sectors = 0;
    int gc_cycles = 0;
    int gc_sectors_read = 0;
    int gc_sectors_written = 0;
    int gc_deleted = 0;

    object_reader *parser;
    
    /* maybe use insertion sorted vector?
     *  https://stackoverflow.com/questions/15843525/how-do-you-insert-the-value-in-a-sorted-vector
     */

    void write_checkpoint(int seq);

    sector_t make_gc_hdr(char *buf, uint32_t seq, sector_t sectors,
			 data_map *extents, int n_extents);

    void do_gc(std::unique_lock<std::mutex> &lk);
    void gc_thread(thread_pool<int> *p);
    void process_batch(batch *b, std::unique_lock<std::mutex> &lk);
    void worker_thread(thread_pool<batch*> *p);
    void ckpt_thread(thread_pool<int> *p);
    void flush_thread(thread_pool<int> *p);

    backend *objstore;

    /* all objects seq<next_compln have been completed
     */
    void notify_complete(int _seq) {
	std::unique_lock lk(m);
	auto moved = false;
	done[_seq % 128] = true;
	while (done[next_compln % 128]) {
	    done[next_compln % 128] = false;
	    next_compln++;
	    moved = true;
	}
	if (moved) 
	    cv.notify_all();
    }

public:
    translate_impl(backend *_io, lsvd_config *cfg_,
		   extmap::objmap *map, std::shared_mutex *m);
    ~translate_impl();

    ssize_t init(const char *name, int nthreads, bool timedflush);
    void shutdown(void);

    int flush(void);            /* write out current batch */
    int checkpoint(void);       /* flush, then write checkpoint */

    ssize_t writev(size_t offset, iovec *iov, int iovcnt);
    void wait_for_room(void);
    ssize_t readv(size_t offset, iovec *iov, int iovcnt);

    const char *prefix() { return single_prefix; }
    
    /* debug functions
     */
    void getmap(int base, int limit,
                int (*cb)(void *ptr,int,int,int,int), void *ptr);
    int mapsize(void);
    void reset(void);
    int frontier(void);
    int batch_seq(void);
};

translate_impl::translate_impl(backend *_io, lsvd_config *cfg_,
			       extmap::objmap *map_, std::shared_mutex *m_) :
    done(128,false), workers(&m), misc_threads(&m) {
    objstore = _io;
    parser = new object_reader(objstore);
    map = map_;
    map_lock = m_;
    cfg = cfg_;
}

translate *make_translate(backend *_io, lsvd_config *cfg,
			  extmap::objmap *map, std::shared_mutex *m) {
    return (translate*) new translate_impl(_io, cfg, map, m);
}

translate_impl::~translate_impl() {
    if (b) 
	delete b;
    delete parser;
    if (super_buf)
	free(super_buf);
}

ssize_t translate_impl::init(const char *prefix_,
			     int nthreads, bool timedflush) {
    std::vector<uint32_t>    ckpts;
    std::vector<clone_info*> clones;
    std::vector<snap_info>   snaps;

    /* note prefix = superblock name
     */
    strcpy(single_prefix, prefix_);
    super_name = single_prefix;

    auto [_buf, bytes] = parser->read_super(super_name, ckpts, clones,
					    snaps, uuid);
    if (bytes < 0)
	return bytes;
    
    super_buf = _buf;
    super_h = (obj_hdr*)super_buf;
    super_len = super_h->hdr_sectors * 512;
    super_sh = (super_hdr*)(super_h+1);

    memcpy(&uuid, super_h->vol_uuid, sizeof(uuid));
    
    seq = next_compln = super_sh->next_obj;
    b = new batch(cfg->batch_size);
    
    int _ckpt = 1;
    for (auto ck : ckpts) {
	ckpts.resize(0);
	std::vector<ckpt_obj> objects;
	std::vector<deferred_delete> deletes;
	std::vector<ckpt_mapentry> entries;
	objname name(prefix(), ck);
	if (parser->read_checkpoint(name.c_str(), ckpts, objects,
				    deletes, entries) < 0)
	    return -1;
	for (auto o : objects) {
	    object_info[o.seq] = (obj_info){.hdr = (int)o.hdr_sectors,
					    .data = (int)o.data_sectors,
					    .live = (int)o.live_sectors,
					    .type = LSVD_DATA};
	    total_sectors += o.data_sectors;
	    total_live_sectors += o.live_sectors;
	}
	for (auto m : entries) {
	    map->update(m.lba, m.lba + m.len,
			    (extmap::obj_offset){.obj = m.obj,
				    .offset = m.offset});
	}
	_ckpt = ck;
    }

    for (int i = _ckpt; ; i++) {
	std::vector<uint32_t>    ckpts;
	std::vector<obj_cleaned> cleaned;
	std::vector<data_map>    entries;
	obj_hdr h; obj_data_hdr dh;
	seq = next_compln = i;
	objname name(prefix(), i);
	if (parser->read_data_hdr(name.c_str(), h, dh, ckpts,
				  cleaned, entries) < 0)
	    break;
	object_info[i] = (obj_info){.hdr = (int)h.hdr_sectors,
				    .data = (int)h.data_sectors,
				    .live = (int)h.data_sectors,
				    .type = LSVD_DATA};
	total_sectors += h.data_sectors;
	total_live_sectors += h.data_sectors;
	int offset = 0, hdr_len = h.hdr_sectors;
	for (auto m : entries) {
	    std::vector<extmap::lba2obj> deleted;
	    extmap::obj_offset oo = {i, offset + hdr_len};
	    map->update(m.lba, m.lba + m.len, oo, &deleted);
	    offset += m.len;
	    for (auto d : deleted) {
		auto [base, limit, ptr] = d.vals();
		object_info[ptr.obj].live -= (limit - base);
		assert(object_info[ptr.obj].live >= 0);
		total_live_sectors -= (limit - base);
	    }
	}
    }

    for (int i = 0; i < nthreads; i++) 
	workers.pool.push(std::thread(&translate_impl::worker_thread,
				      this, &workers));
    misc_threads.pool.push(std::thread(&translate_impl::ckpt_thread,
				       this, &misc_threads));
    if (timedflush)
	misc_threads.pool.push(std::thread(&translate_impl::flush_thread,
					   this, &misc_threads));
    misc_threads.pool.push(std::thread(&translate_impl::gc_thread,
				       this, &misc_threads));

    return bytes;
}

void translate_impl::shutdown(void) {
}

/* ----------- parsing and serializing various objects -------------*/

/* read object header
 *  fast: just read first 4KB
 *  !fast: read first 4KB, resize and read again if >4KB
 */


/* create header for a GC object
 */
sector_t translate_impl::make_gc_hdr(char *buf, uint32_t _seq, sector_t sectors,
				     data_map *extents, int n_extents) {
    auto h = (obj_hdr*)buf;
    auto dh = (obj_data_hdr*)(h+1);
    uint32_t o1 = sizeof(*h) + sizeof(*dh), l1 = sizeof(uint32_t),
	o2 = o1 + l1, l2 = n_extents * sizeof(data_map),
	hdr_bytes = o2 + l2;
    sector_t hdr_sectors = div_round_up(hdr_bytes, 512);

    *h = (obj_hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_DATA, .seq = _seq,
		   .hdr_sectors = (uint32_t)hdr_sectors,
		   .data_sectors = (uint32_t)sectors};
    memcpy(h->vol_uuid, &uuid, sizeof(uuid_t));

    *dh = (obj_data_hdr){.last_data_obj = _seq, .ckpts_offset = o1,
			 .ckpts_len = l1,
			 .objs_cleaned_offset = 0, .objs_cleaned_len = 0,
			 .data_map_offset = o2, .data_map_len = l2};

    uint32_t *p_ckpt = (uint32_t*)(dh+1);
    *p_ckpt = last_ckpt;

    data_map *dm = (data_map*)(p_ckpt+1);
    for (int i = 0; i < n_extents; i++)
	*dm++ = extents[i];

    assert(hdr_bytes == ((char*)dm - buf));
    memset(buf + hdr_bytes, 0, 512*hdr_sectors - hdr_bytes); // valgrind

    return hdr_sectors;
}

/* ----------- data transfer logic -------------*/

/* NOTE: offset is in bytes
 */
ssize_t translate_impl::writev(size_t offset, iovec *iov, int iovcnt) {
    std::unique_lock<std::mutex> lk(m);
    smartiov siov(iov, iovcnt);
    size_t len = siov.bytes();

    if (b->len + len > b->max) {
	b->seq = last_sent = seq++;
	workers.put_locked(b);
	b = new batch(cfg->batch_size);
    }

    b->append(offset / 512, &siov);

    return len;
}

void translate_impl::wait_for_room(void) {
    std::unique_lock<std::mutex> lk(m);
    while (next_compln - last_sent > cfg->xlate_window)
	cv.wait(lk);
}

class translate_req : public trivial_request {
    uint32_t seq;
    translate_impl *tx;
    friend class translate_impl;
    
    /* various things that we might need to free
     */
    std::vector<char*> to_free;
    translate_impl::batch *b = NULL;
    
public:
    translate_req(uint32_t seq_, translate_impl *tx_) {
	seq = seq_;
	tx = tx_;
    }
    ~translate_req(){}

    void notify(request *child) {
	if (child)
	    child->release();
	tx->notify_complete(seq);
	for (auto ptr : to_free)
	    free(ptr);
	if (b) 
	    delete b;
	delete this;
    }
};

void translate_impl::process_batch(batch *b, std::unique_lock<std::mutex> &lk) {
    /* TODO: coalesce writes: 
     *   bufmap m
     *   for e in entries
     *      m.insert(lba, len, ptr)
     *   for e in m:
     *      (lba, iovec) -> output
     */

    /* make the following updates:
     * - object_info - hdrlen, total/live data sectors
     * - map - LBA to obj/offset map
     * - object_info, totals - adjust for new garbage
     */
    size_t hdr_bytes = obj_hdr_len(b->entries.size(), last_ckpt);
    int hdr_sectors = div_round_up(hdr_bytes, 512);

    std::unique_lock objlock(*map_lock);
    obj_info oi = {.hdr = hdr_sectors, .data = (int)b->len/512,
		   .live = (int)b->len/512, .type = LSVD_DATA};
    object_info[b->seq] = oi;

    /* note that we update the map before the object is written,
     * and count on the write cache preventing any reads until
     * it's persisted. TODO: verify this
     */
    sector_t sector_offset = hdr_sectors;
    std::vector<extmap::lba2obj> deleted;

    for (auto e : b->entries) {
	extmap::obj_offset oo = {b->seq, sector_offset};
	map->update(e.lba, e.lba+e.len, oo, &deleted);
	sector_offset += e.len;
    }

    for (auto d : deleted) {
	auto [base, limit, ptr] = d.vals();
	assert(object_info.find(ptr.obj) != object_info.end());
	object_info[ptr.obj].live -= (limit - base);
	assert(object_info[ptr.obj].live >= 0);
	total_live_sectors -= (limit - base);
    }
    objlock.unlock();

    total_sectors += b->len/512;
    total_live_sectors += b->len/512;
    if (next_compln == -1)
	next_compln = b->seq;
    lk.unlock();

    char *hdr = (char*)calloc(hdr_sectors*512, 1);
    make_data_hdr(hdr, b->len, last_ckpt, &b->entries, b->seq, &uuid);
    iovec iov[] = {{hdr, (size_t)(hdr_sectors*512)},
		   {b->buf, b->len}};

    /* on completion, t_req calls notify_complete and frees stuff
     */
    auto t_req = new translate_req(b->seq, this);
    t_req->to_free.push_back(hdr);
    t_req->b = b;
	
    objname name(prefix(), b->seq);
    auto req = objstore->make_write_req(name.c_str(), iov, 2);
    req->run(t_req);
}

/* worker: pull batches from queue and write them out
 */
void translate_impl::worker_thread(thread_pool<batch*> *p) {
    pthread_setname_np(pthread_self(), "worker_thread");

    while (p->running) {
	std::unique_lock<std::mutex> lk(m);
	batch *b;
	if (!p->get_locked(lk, b)) 
	    return;

	process_batch(b, lk);
    }
}

/* flushes any data buffered in current batch, and blocks until all 
 * outstanding writes are complete.
 * returns sequence number of last written object.
 */
int translate_impl::flush() {
    std::unique_lock<std::mutex> lk(m);
    
    if (b->len > 0) {
	b->seq = last_sent = seq++;
	workers.put_locked(b);
	b = new batch(cfg->batch_size);
    }
    auto _seq = last_sent;

    while (next_compln <= _seq)
	cv.wait(lk);

    return _seq;
}

/* wake up every @wait_time, if data is pending in current batch
 * for @timeout then write it to backend
 */
void translate_impl::flush_thread(thread_pool<int> *p) {
    pthread_setname_np(pthread_self(), "flush_thread");
    auto wait_time = std::chrono::milliseconds(500);
    auto timeout = std::chrono::seconds(2);
    auto t0 = std::chrono::system_clock::now();
    auto seq0 = seq.load();

    while (p->running) {
	std::unique_lock<std::mutex> lk(*p->m);
	p->cv.wait_for(lk, wait_time);
	if (p->running && seq0 == seq.load() && b->len > 0) {
	    if (std::chrono::system_clock::now() - t0 > timeout) {
		lk.unlock();
		flush();
	    }
	}
	else {
	    seq0 = seq.load();
	    t0 = std::chrono::system_clock::now();
	}
    }
}

/* -------------- Checkpointing -------------- */

/* synchronously write a checkpoint
 */
void translate_impl::write_checkpoint(int ckpt_seq) {
    std::vector<ckpt_mapentry> entries;
    std::vector<ckpt_obj> objects;

    /* hold the translation layer lock until we get a copy of object_info
     */
    std::unique_lock lk(m);

    /* hold the map lock while we get a copy of the map.
     */
    std::unique_lock objlock(*map_lock);

    last_ckpt = ckpt_seq;
    for (auto it = map->begin(); it != map->end(); it++) {
	auto [base, limit, ptr] = it->vals();
	entries.push_back((ckpt_mapentry){.lba = base,
		    .len = limit-base, .obj = (int32_t)ptr.obj,
		    .offset = (int32_t)ptr.offset});
    }
    objlock.unlock();

    size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);

    for (auto it = object_info.begin(); it != object_info.end(); it++) {
	auto obj_num = it->first;
	auto [hdr, data, live, type] = it->second;
	if (type == LSVD_DATA)
	    objects.push_back((ckpt_obj){.seq = (uint32_t)obj_num,
			.hdr_sectors = (uint32_t)hdr,
			.data_sectors = (uint32_t)data,
			.live_sectors = (uint32_t)live});
    }

    /* add object for this checkpoint
     */
    size_t objs_bytes = objects.size() * sizeof(ckpt_obj);
    size_t hdr_bytes = sizeof(obj_hdr) + sizeof(obj_ckpt_hdr);
    int sectors = div_round_up(hdr_bytes + sizeof(ckpt_seq) + map_bytes +
			       objs_bytes, 512);
    object_info[ckpt_seq] = (obj_info){.hdr = sectors, .data = 0, .live = 0,
				   .type = LSVD_CKPT};
    checkpoints.push(ckpt_seq);
    
    /* wait until all prior objects have been acked by backend
     */
    while (next_compln < ckpt_seq)
	cv.wait(lk);
    lk.unlock();

    /* put it all together in memory
     */
    auto buf = (char*)calloc(hdr_bytes, 1);
    auto h = (obj_hdr*)buf;
    *h = (obj_hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_CKPT, .seq = (uint32_t)ckpt_seq,
		   .hdr_sectors = (uint32_t)sectors, .data_sectors = 0};
    memcpy(h->vol_uuid, uuid, sizeof(uuid_t));
    auto ch = (obj_ckpt_hdr*)(h+1);

    uint32_t o1 = sizeof(obj_hdr)+sizeof(obj_ckpt_hdr), o2 = o1 + sizeof(ckpt_seq),
	o3 = o2 + objs_bytes;
    *ch = (obj_ckpt_hdr){.ckpts_offset = o1, .ckpts_len = sizeof(ckpt_seq),
			 .objs_offset = o2, .objs_len = o3-o2,
			 .deletes_offset = 0, .deletes_len = 0,
			 .map_offset = o3, .map_len = (uint32_t)map_bytes};

    iovec iov[] = {{.iov_base = buf, .iov_len = hdr_bytes},
		   {.iov_base = (char*)&ckpt_seq, .iov_len = sizeof(ckpt_seq)},
		   {.iov_base = (char*)objects.data(), objs_bytes},
		   {.iov_base = (char*)entries.data(), map_bytes}};

    /* and write it
     */
    objname name(prefix(), ckpt_seq);
    objstore->write_object(name.c_str(), iov, 4);
    notify_complete(ckpt_seq);
    
    free(buf);

    /* Now re-write the superblock with the new list of checkpoints
     */
    size_t offset = sizeof(*super_h) + sizeof(*super_sh);

    /* this is the only place we modify *super_sh
     */
    super_sh->ckpts_offset = offset;
    super_sh->ckpts_len = sizeof(ckpt_seq);
    *(int*)(super_buf + offset) = ckpt_seq;
    struct iovec iov2 = {super_buf, 4096};
    objstore->write_object(super_name, &iov2, 1);
}

void translate_impl::ckpt_thread(thread_pool<int> *p) {
    const char *name = "ckpt_thread";
    pthread_setname_np(pthread_self(), name);
    auto one_second = std::chrono::seconds(1);
    auto seq0 = seq.load();
    const int ckpt_interval = 100;

    while (p->running) {
	std::unique_lock<std::mutex> lk(m);
	p->cv.wait_for(lk, one_second);
	if (p->running && seq.load() - seq0 > ckpt_interval) {
	    seq0 = seq.load();
	    lk.unlock();
	    checkpoint();
	}
    }
}

int translate_impl::checkpoint(void) {
    std::unique_lock<std::mutex> lk(m);
    if (b->len > 0) {
	b->seq = seq++;
	workers.put_locked(b);
	b = new batch(cfg->batch_size);
    }
    int _seq = seq++;
    lk.unlock();
    write_checkpoint(_seq);
    return _seq;
}


/* -------------- Garbage collection ---------------- */

/* Needs a lot of work. Note that all reads/writes are synchronous
 * for now...
 */
void translate_impl::do_gc(std::unique_lock<std::mutex> &lk) {
    assert(!m.try_lock());	// must be locked
    gc_cycles++;
    int max_obj = seq.load();

    /* create list of object info in increasing order of 
     * utilization, i.e. (live data) / (total size)
     */
    std::set<std::tuple<double,int,int>> utilization;

    for (auto p : object_info)  {
	auto [hdrlen, datalen, live, type] = p.second;
	if (type != LSVD_DATA)
	    continue;
	double rho = 1.0 * live / datalen;
	sector_t sectors = hdrlen + datalen;
	utilization.insert(std::make_tuple(rho, p.first, sectors));
	assert(sectors <= 10*1024*1024/512);
    }

    /* gather list of objects needing cleaning, return if none
     */
    const double threshold = 0.50;
    std::vector<std::pair<int,int>> objs_to_clean;
    for (auto [u, o, n] : utilization) {
	if (u > threshold)
	    break;
	if (objs_to_clean.size() > 32)
	    break;
	objs_to_clean.push_back(std::make_pair(o, n));
    }
    if (objs_to_clean.size() == 0) 
	return;
	
    /* find all live extents in objects listed in objs_to_clean:
     * - make bitmap from objs_to_clean
     * - find all entries in map pointing to those objects
     */
    std::vector<bool> bitmap(max_obj+1);
    for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++)
	bitmap[it->first] = true;

    std::unique_lock objlock(*map_lock); // TODO: might not need til later...
    extmap::objmap live_extents;
    for (auto it = map->begin(); it != map->end(); it++) {
	auto [base, limit, ptr] = it->vals();
	if (bitmap[ptr.obj])
	    live_extents.update(base, limit, ptr);
    }
    objlock.unlock();
    lk.unlock();

    /* everything before this point was in-memory only, with the 
     * translation instance mutex held, doing no I/O.
     */

    /* this is really gross. We really should check to see if data is
     * in the read cache, and put retrieved data there...
     */
    if (live_extents.size() > 0) {
	/* temporary file, delete on close. 
	 */
	char temp[cfg->cache_dir.size() + 20];
	sprintf(temp, "%s/gc.XXXXXX", cfg->cache_dir.c_str());
	int fd = mkstemp(temp);
	unlink(temp);

	/* read all objects in completely. Someday we can check to see whether
	 * (a) data is already in cache, or (b) sparse reading would be quicker
	 */
	extmap::cachemap file_map;
	sector_t offset = 0;
	char *buf = (char*)malloc(10*1024*1024);

	for (auto [i,sectors] : objs_to_clean) {
	    objname name(prefix(), i);
	    iovec iov = {buf, (size_t)(sectors*512)};
	    objstore->read_object(name.c_str(), &iov, 1, /*offset=*/ 0);
	    gc_sectors_read += sectors;
	    extmap::obj_offset _base = {i, 0}, _limit = {i, sectors};
	    file_map.update(_base, _limit, offset);
	    if (write(fd, buf, sectors*512) < 0)
		throw("no space");
	    offset += sectors;
	}
	free(buf);

	struct _extent {
	    int64_t base;
	    int64_t limit;
	    extmap::obj_offset ptr;
	};
	std::vector<_extent> all_extents;
	for (auto it = live_extents.begin(); it != live_extents.end(); it++) {
	    auto [base, limit, ptr] = it->vals();
	    all_extents.push_back((_extent){base, limit, ptr});
	}
	
	while (all_extents.size() > 0) {
	    sector_t sectors = 0, max = 16 * 1024; // 8MB
	    char *hdr = (char*)malloc(1024*32);	// 8MB / 4KB = 2K extents = 16KB

	    auto it = all_extents.begin();
	    while (it != all_extents.end() && sectors < max) {
		auto [base, limit, ptr] = *it++;
		sectors += (limit - base);
		(void)ptr;	// suppress warning
	    }
	    std::vector<_extent> extents(std::make_move_iterator(all_extents.begin()),
					 std::make_move_iterator(it));
	    all_extents.erase(all_extents.begin(), it);
	    
	    /* lock the map while we read from the file. 
	     * TODO: read everything in, put it in a bufmap, and then go 
	     * back and construct an iovec for the subset that's still valid.
	     */
	    char *buf = (char*)aligned_alloc(512, sectors * 512);

	    lk.lock();		// m
	    objlock.lock();	// *map_lock
	    off_t byte_offset = 0;
	    sector_t data_sectors = 0;
	    std::vector<data_map> obj_extents;
	    
	    for (auto [base, limit, ptr] : extents) {
		for (auto it2 = map->lookup(base);
		     it2->base() < limit && it2 != map->end(); it2++) {
		    auto [_base, _limit, _ptr] = it2->vals(base, limit);
		    if (_ptr.obj != ptr.obj)
			continue;
		    sector_t _sectors = _limit - _base;
		    size_t bytes = _sectors*512;
		    auto it3 = file_map.lookup(_ptr);
		    auto file_sector = it3->ptr();

		    auto err = pread(fd, buf+byte_offset, bytes, file_sector*512);
		    if (err != (ssize_t)bytes) {
			printf("\n\n");
			printf("%ld != %ld, obj=%ld end=%s\n", err, bytes, _ptr.obj,
			       it3 == file_map.end() ? "Y" : "n");
			throw_fs_error("gc");
		    }
		    obj_extents.push_back((data_map){(uint64_t)_base, (uint64_t)_sectors});

		    data_sectors += _sectors;
		    byte_offset += bytes;
		}
	    }
	    int32_t _seq = seq++;	    

	    gc_sectors_written += data_sectors;
	    int hdr_sectors = make_gc_hdr(hdr, _seq, data_sectors,
					  obj_extents.data(), obj_extents.size());
	    auto offset = hdr_sectors;

	    int gc_sectors = byte_offset / 512;
	    obj_info oi = {.hdr = hdr_sectors, .data = gc_sectors,
		   .live = gc_sectors, .type = LSVD_DATA};
	    object_info[_seq] = oi;
	    
	    for (auto e : obj_extents) {
		extmap::obj_offset oo = {_seq, offset};
		map->update(e.lba, e.lba+e.len, oo, NULL);
		offset += e.len;
	    }
	    objlock.unlock();
	    lk.unlock();

	    smartiov iovs;
	    iovs.push_back((iovec){hdr, (size_t)hdr_sectors*512});
	    iovs.push_back((iovec){buf, (size_t)byte_offset});

	    auto t_req = new translate_req(_seq, this);
	    t_req->to_free.push_back(hdr);
	    t_req->to_free.push_back(buf);

	    objname name(prefix(), _seq);
	    auto [iov,iovcnt] = iovs.c_iov();
	    auto req = objstore->make_write_req(name.c_str(), iov, iovcnt);
	    req->run(t_req);
	}
	close(fd);
    }

    lk.lock();
    for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++) 
	object_info.erase(object_info.find(it->first));

    /* trim checkpoints
     */
    std::vector<int> ckpts_to_delete;
    while (checkpoints.size() > 3) {
	ckpts_to_delete.push_back(checkpoints.front());
	checkpoints.pop();
    }

    /* write checkpoint *before* deleting any objects
     */
    int ckpt_seq = seq++;
    lk.unlock();
    write_checkpoint(ckpt_seq);
    
    for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++) {
	objname name(prefix(), it->first);
	objstore->delete_object(name.c_str());
	gc_deleted++;		// single-threaded, no lock needed
    }

    for (auto c : ckpts_to_delete) {
	objname name(prefix(), c);
	objstore->delete_object(name.c_str());
    }
    lk.lock();
}


void translate_impl::gc_thread(thread_pool<int> *p) {
    auto interval = std::chrono::milliseconds(100);
    sector_t trigger = 128 * 1024 * 2; // 128 MB
    const char *name = "gc_thread";
    pthread_setname_np(pthread_self(), name);
	
    while (p->running) {
	std::unique_lock lk(m);
	p->cv.wait_for(lk, interval);
	if (!p->running)
	    return;
	if (total_sectors - total_live_sectors < trigger)
	    continue;
	if (((double)total_live_sectors / total_sectors) > 0.6)
	    continue;
	do_gc(lk);
    }
}
    
/* ---------------- Debug ---------------- */

/* synchronous read from offset (in bytes)
 */
ssize_t translate_impl::readv(size_t offset, iovec *iov, int iovcnt) {
    smartiov iovs(iov, iovcnt);
    size_t len = iovs.bytes();
    int64_t base = offset / 512;
    int64_t sectors = len / 512, limit = base + sectors;

    /* various things break when map size is zero
     */
    if (map->size() == 0) {
	iovs.zero();
	return len;
    }

    /* object number, offset (bytes), length (bytes) */
    std::vector<std::tuple<int, size_t, size_t>> regions;
	
    auto prev = base;
    {
	std::unique_lock lk(m);
	std::shared_lock slk(*map_lock);

	for (auto it = map->lookup(base);
	     it != map->end() && it->base() < limit; it++) {
	    auto [_base, _limit, oo] = it->vals(base, limit);
	    if (_base > prev) {	// unmapped
		size_t _len = (_base - prev)*512;
		regions.push_back(std::tuple(-1, 0, _len));
	    }
	    size_t _len = (_limit - _base) * 512, _offset = oo.offset * 512;
	    regions.push_back(std::tuple((int)oo.obj, _offset, _len));
	    prev = _limit;
	}
    }

    if (regions.size() == 0) {
	iovs.zero();
	return 0;
    }
    
    size_t iov_offset = 0;
    for (auto [obj, _offset, _len] : regions) {
	auto slice = iovs.slice(iov_offset, iov_offset + _len);
	if (obj == -1)
	    slice.zero();
	else {
	    objname name(prefix(), obj);
	    auto [iov,iovcnt] = slice.c_iov();
	    objstore->read_object(name.c_str(), iov, iovcnt, _offset);
	}
	iov_offset += _len;
    }

    if (iov_offset < iovs.bytes()) {
	auto slice = iovs.slice(iov_offset, len - iov_offset);
	slice.zero();
    }
    
    return 0;
}

void translate_impl::getmap(int base, int limit,
		       int (*cb)(void *ptr,int,int,int,int), void *ptr) {
    for (auto it = map->lookup(base); it != map->end() && it->base() < limit; it++) {
	auto [_base, _limit, oo] = it->vals(base, limit);
	if (!cb(ptr, (int)_base, (int)_limit, (int)oo.obj, (int)oo.offset))
	    break;
    }
}
int translate_impl::mapsize(void) {
    return map->size();
}
void translate_impl::reset(void) {
    map->reset();
}
int translate_impl::frontier(void) {
    return b->len / 512;
}
int translate_impl::batch_seq(void) {
    return seq;
}

int batch_seq(translate *xlate_) {
    auto xlate = (translate_impl*)xlate_;
    return xlate->batch_seq();
}
