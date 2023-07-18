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
#include <zlib.h>

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
extern void log_time(uint64_t loc, uint64_t val); // debug

/*
 * Architecture:
 *
 * all operations go through a single queue, handled by a single worker
 * thread. All sequence numbers are assigned by this thread, so operations
 * are totally ordered.
 *
 * - writes are aggregated into batches and submitted to the queue. 
 *   objmap entries are updated before sending backend PUT request
 *
 * - checkpoint request: write the current map.
 *
 * - flush: block until all outstanding writes have completed
 *
 * - GC: the GC thread creates "tentative" GC writes, including all data
 *   to be written, and submits to the queue. 
 *   Request processing (a) revalidates all data in the request, 
 *   (b) updates the map, and (c) sends PUT request.
 * 
 * in-memory map: the above steps leave the following windows where data 
 * is not available for reading:
 *
 * 1. between (a1) appending to the current batch and (a2) assigning a 
 *    sequence number - there's no "name" for the location of the data
 * 2. between (a2) assigning a sequence number and (a3) completion of
 *    PUT request - data may not be available for GET requests
 * 3. like 2, but for GC - after (b1) updating the map until (b2) the
 *    new PUT request is durable on the backend
 *
 * During each of these periods the data is guaranteed to be sitting in an
 * in-memory buffer. We keep a map from LBA to in-memory pointers, updating
 * it at points a1 and b1 and removing entries at a3 and b2 (i.e. write
 * request completion), being careful of overwrites in the interim.
 */

/* ----------- Object translation layer -------------- */

enum work_type {
    REQ_PUT = 1,
    REQ_FLUSH = 2,
    REQ_CKPT = 3,
    REQ_GC = 4
};

class translate_impl;

/* TODO: local_buf_base/limit are set in constructor for PUT, 
 * translate_impl::write_gc for GC. this should be uniform...
 */

class translate_req : public request {
    work_type       op;
    translate_impl *tx;
    std::mutex      m;
    std::condition_variable cv;
    bool            done = false;
    
    friend class translate_impl;

    /* REQ_PUT */
    char    *batch_buf = NULL;	// actual allocation
    char    *data_ptr;		// batch_buf+room
    /* entries */
    
    size_t   len = 0;		// in bytes
    size_t   max = 0;

    /* REQ_GC */
    char  *gc_buf = NULL;	// object to write out
    char  *gc_data = NULL;	// passed in by GC thread
    /* entries*/

    /* lba/len/obj/offset (ignore obj/offset for REQ_PUT) */
    std::vector<ckpt_mapentry> entries;

    /* used for removing from map */
    char *local_buf_base = NULL;
    char *local_buf_limit = NULL;

    int           _seq;
    
public:
    char *append(int64_t lba, smartiov *iov) {
	assert(op == REQ_PUT);
	int64_t bytes = iov->bytes();
	entries.push_back((ckpt_mapentry){lba, bytes/512, 0, 0});
	char *ptr = data_ptr + len;
	iov->copy_out(ptr);
	len += bytes;
	return ptr;
    }

    bool room(size_t bytes) {
	assert(op == REQ_PUT);
	return len+bytes <= max;
    }
    
    ~translate_req(){}

    /* NOTE - this assumes the only significant header entry is the map
     */
    translate_req(work_type op_, size_t bytes, translate_impl *tx_) {
	assert (op_ == REQ_PUT);
	op = op_;
	tx = tx_;
	int max_hdr_bytes = 1024 + (bytes / 2048) * sizeof(data_map);
	batch_buf = (char*)malloc(bytes + max_hdr_bytes);
	data_ptr = batch_buf + max_hdr_bytes;
	max = bytes;

	local_buf_base = data_ptr;
	local_buf_limit = data_ptr + bytes;
    }
    
    translate_req(work_type op_, translate_impl *tx_) {
	op = op_;
	tx = tx_;
    }
    
    void wait(void) {
	std::unique_lock lk(m);
	while (!done)
	    cv.wait(lk);
	lk.unlock();
	delete this;		// always call wait
    }

    void notify(request *child);
    void run(request *parent) {} // unused
    void release(void) {}	// unused
};


class translate_impl : public translate {
    /* lock ordering: lock m before *map_lock
     */
    std::mutex         m;	// for things in this instance
    extmap::objmap    *map;	// shared object map
    extmap::bufmap    *bufmap;	// shared object map
    std::shared_mutex *map_lock; // locks the object map
    lsvd_config       *cfg;

    std::atomic<int>   seq;
    uint64_t           ckpt_cache_seq = 0; // from last data object
    
    friend class translate_req;
    translate_req     *current = NULL;
    
    /* info on live data objects - all sizes in sectors 
     * checkpoints are tracked in @checkpoints, and in the superblock
     */
    struct obj_info {
	int hdr;		// sectors
	int data;		// sectors
	int live;		// sectors
    };
    std::map<int,obj_info> object_info;

    std::vector<uint32_t> checkpoints;
    
    std::atomic<int> outstanding_writes = 0;
    
    std::condition_variable cv;
    bool stopped = false;	// stop GC from writing

    /* various constant state
     */
    struct clone {
	char prefix[128];
	int  last_seq;
	int  first_seq = 0;
    };
    std::vector<clone> clone_list;
    char               super_name[128];

    /* superblock has two sections: [obj_hdr] [super_hdr]
     */
    char      *super_buf = NULL;
    obj_hdr   *super_h = NULL;
    super_hdr *super_sh = NULL;
    size_t     super_len;

    std::set<int> objs_written;	// HACK - for wait_object_ready

    thread_pool<translate_req*> *workers;
    thread_pool<int>    *misc_threads; // so we can stop ckpt, gc first

    /* for triggering GC
     */
    sector_t total_sectors = 0;
    sector_t total_live_sectors = 0;
    int gc_cycles = 0;
    int gc_sectors_read = 0;
    int gc_sectors_written = 0;
    int gc_deleted = 0;

    /* for shutdown
     */
    bool gc_running = false;
    std::condition_variable gc_cv;
    void stop_gc(void);
    
    object_reader *parser;
    
    void write_checkpoint(int seq, translate_req *req);
    void process_batch(int seq, translate_req *req);
    void write_gc(int _seq, translate_req *req);
    
    void worker_thread(thread_pool<translate_req*> *p);
    
    sector_t make_gc_hdr(char *buf, uint32_t seq, sector_t sectors,
			 data_map *extents, int n_extents);

    void do_gc(bool *running);
    void gc_thread(thread_pool<int> *p);
    void flush_thread(thread_pool<int> *p);

    backend *objstore;

public:
    translate_impl(backend *_io, lsvd_config *cfg_,
		   extmap::objmap *map, extmap::bufmap *bufmap,
		   std::shared_mutex *m);
    ~translate_impl();

    ssize_t init(const char *name, bool timedflush);
    void shutdown(void);

    void flush(void);            /* write out current batch */
    void checkpoint(void);       /* flush, then write checkpoint */

    ssize_t writev(uint64_t cache_seq, size_t offset, iovec *iov, int iovcnt);
    ssize_t trim(size_t offset, size_t len);
    
    void wait_for_room(void);
    ssize_t readv(size_t offset, iovec *iov, int iovcnt);
    bool wait_object_ready(int obj);
    void start_gc(void);
    
    const char *prefix(int seq);
    
};

const char *translate_impl::prefix(int seq) {
    if (clone_list.size() == 0 || seq > clone_list.front().last_seq)
	return super_name;
    for (auto const & c : clone_list)
	if (seq >= c.first_seq)
	    return c.prefix;
    assert(false);
}

translate_impl::translate_impl(backend *_io, lsvd_config *cfg_,
			       extmap::objmap *map_,
			       extmap::bufmap *bufmap_,
			       std::shared_mutex *m_) {
    misc_threads = new thread_pool<int>(&m);
    workers = new thread_pool<translate_req*>(&m);
    objstore = _io;
    parser = new object_reader(objstore);
    map = map_;
    bufmap = bufmap_;
    map_lock = m_;
    cfg = cfg_;
}

translate *make_translate(backend *_io, lsvd_config *cfg,
			  extmap::objmap *map, extmap::bufmap *bufmap,
			  std::shared_mutex *m) {
    return (translate*) new translate_impl(_io, cfg, map, bufmap, m);
}

translate_impl::~translate_impl() {
    stopped = true;
    cv.notify_all();
    if (current) 
	delete current;
    delete parser;
    if (super_buf)
	free(super_buf);
}

ssize_t translate_impl::init(const char *prefix_, bool timedflush) {
    std::vector<uint32_t>    ckpts;
    std::vector<clone_info*> clones;
    std::vector<snap_info*>  snaps;

    /* note prefix = superblock name
     */
    strcpy(super_name, prefix_);

    auto [_buf, bytes] = parser->read_super(super_name, ckpts, clones,
					    snaps, uuid);
    if (bytes < 0)
	return bytes;
    int n_ckpts = ckpts.size();
    
    super_buf = _buf;
    super_h = (obj_hdr*)super_buf;
    super_len = super_h->hdr_sectors * 512;
    super_sh = (super_hdr*)(super_h+1);

    memcpy(&uuid, super_h->vol_uuid, sizeof(uuid));

    current = new translate_req(REQ_PUT, cfg->batch_size, this);
    seq = 1;			// empty volume case
    
    /* is this a clone?
     */
    if (super_sh->clones_len > 0) {
	char buf[4096];
	auto ci = (clone_info*)(_buf + super_sh->clones_offset);
	auto obj_name = (char*)(ci+1);
	while (true) {
	    if (objstore->read_object(obj_name, buf, sizeof(buf), 0) < 0)
		return -1;
	    auto _h = (obj_hdr*)buf;
	    auto _sh = (super_hdr*)(_h+1);
	    if (_h->magic != LSVD_MAGIC || _h->type != LSVD_SUPER) {
		printf("clone: bad magic\n");
		return -1;
	    }
	    if (memcmp(_h->vol_uuid, ci->vol_uuid, sizeof(uuid_t)) != 0) {
		printf("clone: bad UUID\n");
		return -1;
	    }
	    clone c;
	    strcpy(c.prefix, obj_name);
	    c.last_seq = ci->last_seq;
	    if (clone_list.size() > 0)
		clone_list.back().first_seq = ci->last_seq + 1;
	    clone_list.push_back(c);

	    if (_sh->clones_len == 0)
		break;
	    ci = (clone_info*)(buf + _sh->clones_offset);
	    obj_name = (char*)(ci + 1);
	}
    }
    
    /* read in the last checkpoint, then roll forward from there;
     */
    int last_ckpt = -1;
    if (ckpts.size() > 0) {
	std::vector<ckpt_obj> objects;
	std::vector<deferred_delete> deletes;
	std::vector<ckpt_mapentry> entries;

	/* hmm, we should never have checkpoints listed in the
	 * super that aren't persisted on the backend, should we?
	 */
	while (n_ckpts > 0) {
	    int c = ckpts[n_ckpts-1];
	    objname name(prefix(c), c);
	    do_log("reading ckpt %s\n", name.c_str());
	    if (parser->read_checkpoint(name.c_str(), max_cache_seq,
					ckpts, objects,
					deletes, entries) >= 0) {
		last_ckpt = c;
		break;
	    }
	    do_log("chkpt skip %d\n", c);
	    n_ckpts--;
	}
	if (last_ckpt == -1)
	    return -1;

	for (int i = 0; i < n_ckpts; i++) {
	    do_log("chkpt from super: %d\n", ckpts[i]);
	    checkpoints.push_back(ckpts[i]); // so we can delete them later
	}

	for (auto o : objects) {
	    object_info[o.seq] = (obj_info){.hdr = (int)o.hdr_sectors,
					    .data = (int)o.data_sectors,
					    .live = (int)o.live_sectors};
	    total_sectors += o.data_sectors;
	    total_live_sectors += o.live_sectors;
	}
	for (auto m : entries) {
	    map->update(m.lba, m.lba + m.len,
			    (extmap::obj_offset){.obj = m.obj,
				    .offset = m.offset});
	}
	seq = last_ckpt + 1;
    }

    /* roll forward
     */
    for (; ; seq++) {
	std::vector<obj_cleaned> cleaned;
	std::vector<data_map>    entries;
	obj_hdr h; obj_data_hdr dh;

	objname name(prefix(seq), seq);
	if (parser->read_data_hdr(name.c_str(), h, dh, cleaned, entries) < 0)
	    break;
	if (h.type == LSVD_CKPT) {
	    do_log("ckpt from roll-forward: %d\n", seq.load());
	    checkpoints.push_back(seq);
	    continue;
	}

	do_log("roll %d\n", seq.load());
	assert(h.type == LSVD_DATA);
	object_info[seq] = (obj_info){.hdr = (int)h.hdr_sectors,
				      .data = (int)h.data_sectors,
				      .live = (int)h.data_sectors};
	total_sectors += h.data_sectors;
	total_live_sectors += h.data_sectors;
	if (dh.cache_seq)	// skip GC writes
	    max_cache_seq = dh.cache_seq;
	
	int offset = 0, hdr_len = h.hdr_sectors;
	std::vector<extmap::lba2obj> deleted;
	for (auto m : entries) {
	    extmap::obj_offset oo = {seq, offset + hdr_len};
	    map->update(m.lba, m.lba + m.len, oo, &deleted);
	    offset += m.len;
	}
	for (auto d : deleted) {
	    auto [base, limit, ptr] = d.vals();
	    object_info[ptr.obj].live -= (limit - base);
	    assert(object_info[ptr.obj].live >= 0);
	    total_live_sectors -= (limit - base);
	}
    }

    /* delete any potential "dangling" objects.
     */
    for (int i = 1; i < 32; i++) {
	objname name(prefix(i+seq), i + seq);
	objstore->delete_object(name.c_str());
    }

    workers->pool.push(std::thread(&translate_impl::worker_thread,
				   this, workers));
    if (timedflush)
	misc_threads->pool.push(std::thread(&translate_impl::flush_thread,
					    this, misc_threads));
    return bytes;
}

void translate_impl::start_gc(void) {
    misc_threads->pool.push(std::thread(&translate_impl::gc_thread,
				       this, misc_threads));
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
    uint32_t o1 = sizeof(*h) + sizeof(*dh),
	l1 = n_extents * sizeof(data_map),
	hdr_bytes = o1 + l1;
    sector_t hdr_sectors = div_round_up(hdr_bytes, 512);

    *h = (obj_hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_DATA, .seq = _seq,
		   .hdr_sectors = (uint32_t)hdr_sectors,
		   .data_sectors = (uint32_t)sectors, .crc = 0};
    memcpy(h->vol_uuid, &uuid, sizeof(uuid_t));

    *dh = (obj_data_hdr){.cache_seq = 0,
			 .objs_cleaned_offset = 0, .objs_cleaned_len = 0,
			 .data_map_offset = o1, .data_map_len = l1,
			 .is_gc = 1};

    data_map *dm = (data_map*)(dh+1);
    for (int i = 0; i < n_extents; i++)
	*dm++ = extents[i];

    assert(hdr_bytes == ((char*)dm - buf));
    memset(buf + hdr_bytes, 0, 512*hdr_sectors - hdr_bytes); // valgrind
    h->crc = (uint32_t)crc32(0, (const unsigned char*)buf, 512*hdr_sectors);
    
    return hdr_sectors;
}

/* ----------- data transfer logic -------------*/

/* NOTE: offset is in bytes
 */
ssize_t translate_impl::writev(uint64_t cache_seq, size_t offset,
			       iovec *iov, int iovcnt) {
    smartiov siov(iov, iovcnt);
    size_t bytes = siov.bytes();
    sector_t base = offset / 512, limit = (offset + bytes) / 512;

    std::unique_lock lk(m);
    if (!current->room(bytes)) {
	workers->put_locked(current);
	current = new translate_req(REQ_PUT, cfg->batch_size, this);
    }
    
    /* save extent and pointer into bufmap. 
     */
    auto ptr = current->append(base, &siov);
    std::unique_lock obj_w_lock(*map_lock);
    bufmap->update(base, limit, ptr);

    return 0;
}

/* TRIM is not guaranteed durable, because:
 * 1. it doesn't remove data in the write cache
 * 2. it doesn't remove data in the write pipeline
 * 3. it's not logged, so the map update isn't persistent until the 
 *    next checkpoint
 *
 * to implement durable discard we would need to:
 * - flush write cache map entries
 * - insert the discard, in order, into a batch
 * - persist it in the object data header
 * possible solution: reserve a tombstone bit in struct data_map
 * TODO: do we need durable discard?
 */
ssize_t translate_impl::trim(size_t offset, size_t len) {
    std::unique_lock lk(m);
    std::unique_lock obj_w_lock(*map_lock);

    // trim the map
    std::vector<extmap::lba2obj> deleted;
    map->trim(offset/512, (offset+len)/512, &deleted);

    // and then update the GC accounting
    for (auto d : deleted) {
	auto [base, limit, ptr] = d.vals();
	if (object_info.find(ptr.obj) == object_info.end())
	    continue;		// skip clone base
	object_info[ptr.obj].live -= (limit - base);
	assert(object_info[ptr.obj].live >= 0);
	total_live_sectors -= (limit - base);
    }
    return 0;
}

void translate_impl::wait_for_room(void) {
    std::unique_lock lk(m);
    while (outstanding_writes > cfg->xlate_window)
	cv.wait(lk);
}

bool translate_impl::wait_object_ready(int obj) {
    std::unique_lock lk(m);
    while (objs_written.find(obj) == objs_written.end())
	cv.wait(lk);
}

/* NOTE - currently not called for REQ_CKPT, which
 * uses sync write
 */
void translate_req::notify(request *child) {
    if (child)
	child->release();

    if (op == REQ_PUT || op == REQ_GC) {
	/* wake up anyone waiting for TX window room
	 * -> lock tx->m before tx->map_lock
	 * also wakes up wait_object_ready
	 */
	std::unique_lock lk(tx->m);
	//if (--tx->outstanding_writes < tx->cfg->xlate_window)
	tx->outstanding_writes--;
	tx->objs_written.insert(_seq);
	tx->cv.notify_all();

	/* remove extents from tx->bufmap, but only if they still
	 * point to this buffer
	 */
	std::unique_lock obj_w_lock(*tx->map_lock);
	std::vector<std::pair<sector_t,sector_t>> extents;
	for (auto const & e : entries) {
	    auto limit = e.lba+e.len;
	    for (auto it2 = tx->bufmap->lookup(e.lba);
		 it2 != tx->bufmap->end() && it2->base() < limit; it2++) {
		auto [_base, _limit, ptr] = it2->vals(e.lba, limit);
		if (ptr.buf >= local_buf_base && ptr.buf < local_buf_limit)
		    extents.push_back(std::pair(_base, _limit));
	    }
	}
	for (auto [base, limit] : extents)
	    tx->bufmap->trim(base, limit);
    }

    if (batch_buf != NULL)	// allocated in constructor
	free(batch_buf);
    if (gc_buf != NULL)		// allocated in write_gc
	free(gc_buf);
    if (gc_data != NULL)	// allocated in gc threqad
	free(gc_data);
	
    if (op == REQ_GC) {
	std::unique_lock lk(m);
	done = true;
	cv.notify_one();
    }
    else 
	delete this;
}

/* write out a checkpoint. Note that we don't have to lock the objmap,
 * since no one else is modifying it right now.
 *
 * possible changes:
 * - wait for preceding writes to complete before writing?
 * - write async rather than sync? (not really compatible with prev)
 */
void translate_impl::write_checkpoint(int _seq, translate_req *req) {
    std::vector<ckpt_mapentry> entries;
    std::vector<ckpt_obj> objects;

    for (auto it = map->begin(); it != map->end(); it++) {
	auto [base, limit, ptr] = it->vals();
	entries.push_back((ckpt_mapentry){.lba = base,
		    .len = limit-base, .obj = (int32_t)ptr.obj,
		    .offset = (int32_t)ptr.offset});
    }
    size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);

    for (auto it = object_info.begin(); it != object_info.end(); it++) {
	auto obj_num = it->first;
	auto [hdr, data, live] = it->second;
	objects.push_back((ckpt_obj){.seq = (uint32_t)obj_num,
		    .hdr_sectors = (uint32_t)hdr,
		    .data_sectors = (uint32_t)data,
		    .live_sectors = (uint32_t)live});
    }
    size_t objs_bytes = objects.size() * sizeof(ckpt_obj);

    size_t hdr_bytes = sizeof(obj_hdr) + sizeof(obj_ckpt_hdr);
    int sectors = div_round_up(hdr_bytes + map_bytes + objs_bytes, 512);

    auto buf = (char*)calloc(sectors*512, 1);
    auto h = (obj_hdr*)buf;
    *h = (obj_hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_CKPT, .seq = (uint32_t)_seq,
		   .hdr_sectors = (uint32_t)sectors, .data_sectors = 0};
    memcpy(h->vol_uuid, uuid, sizeof(uuid_t));
    auto ch = (obj_ckpt_hdr*)(h+1);

    uint32_t o1 = sizeof(obj_hdr)+sizeof(obj_ckpt_hdr), o2 = o1 + objs_bytes;
    *ch = (obj_ckpt_hdr){.cache_seq = ckpt_cache_seq,
			 .ckpts_offset = 0, .ckpts_len = 0,
			 .objs_offset = o1, .objs_len = o2-o1,
			 .deletes_offset = 0, .deletes_len = 0,
			 .map_offset = o2, .map_len = (uint32_t)map_bytes};

    auto objs = (char*)(ch+1);
    memcpy(objs, (char*)objects.data(), objs_bytes);
    auto maps = objs + objs_bytes;
    memcpy(maps, (char*)entries.data(), map_bytes);
    
    /* and write it
     */
    objname name(prefix(_seq), _seq);
    objstore->write_object(name.c_str(), buf, sectors*512);
    free(buf);

    req->done = true;
    req->cv.notify_all();
}



/* 
 * handle a "tentative" garbage collection request.
 *
 * the request has a list of LBA ranges, the object ranges the
 * ranges pointed to when they were selected for GC, and the data 
 * those locations contained at that time.
 *
 * writes are not blocked while GC is building these requests, so
 * some ranges may have been overwritten. 
 *
 * to process the request: (a) revalidate all mappings, discarding 
 * any which have changed, and (b) assign a sequence number
 * this guarantees that the contents reflect the map state after all
 * previous seq#s and before all following ones.
 */
void translate_impl::write_gc(int _seq, translate_req *req) {
    req->_seq = _seq;
    
    int data_sectors = 0;
    for (const auto &e : req->entries)
	data_sectors += e.len;

    int max_hdr_bytes = sizeof(obj_hdr) + sizeof(obj_data_hdr) +
	(cfg->batch_size / 2048) * sizeof(data_map);
    int max_hdr_sectors = div_round_up(max_hdr_bytes, 512);

    auto buf = req->gc_buf = (char*)malloc((max_hdr_sectors+data_sectors)*512);
    memset(buf, 0, max_hdr_sectors*512);
    auto data_ptr = buf + max_hdr_sectors*512;
    auto data_ptr0 = data_ptr;
    auto in_ptr = req->local_buf_base = req->gc_data;
    std::vector<data_map> obj_extents;

    int _data_sectors = 0;	// actual sectors in GC write
    
    for (auto const & [base, len, obj, offset] : req->entries) {
	auto limit = base+len;
	for (auto it2 = map->lookup(base);
	     it2 != map->end() && it2->base() < limit; it2++) {
	     /* [_base,_limit] is a piece of the extent
	      * obj_base is where that piece starts in the object
	      */
	    auto [_base, _limit, ptr] = it2->vals(base, limit);
	    if (ptr.obj != obj)
		continue;

	    sector_t _sectors = _limit - _base;
	    _data_sectors += _sectors;
	    int bytes = _sectors*512;

	    sector_t extent_offset = _base - base;
	    memcpy(data_ptr, in_ptr + extent_offset*512, bytes);
	    data_ptr += bytes;
	    obj_extents.push_back((data_map){
		    (uint64_t)_base, (uint64_t)(_limit-_base)});
	}
	in_ptr += len*512;
    }
    req->local_buf_limit = in_ptr;
    data_sectors = (data_ptr - data_ptr0) / 512;

    int hdr_bytes = sizeof(obj_hdr) + sizeof(obj_data_hdr) +
	obj_extents.size() * sizeof(data_map);
    int hdr_sectors = div_round_up(hdr_bytes, 512);
    
    sector_t offset = hdr_sectors;
    data_ptr = data_ptr0;
    std::vector<extmap::lba2obj> deleted;

    std::unique_lock obj_w_lock(*map_lock); // protect the readers
    for (auto const &e : obj_extents) {
	extmap::obj_offset oo = {_seq, offset};
	map->update(e.lba, e.lba+e.len, oo, &deleted);
	offset += e.len;
	bufmap->update(e.lba, e.lba+e.len, data_ptr);
	data_ptr += e.len*512;
    }
    obj_w_lock.unlock();
    
    for (auto &d : deleted) {
	auto [base, limit, ptr] = d.vals();
	if (object_info.find(ptr.obj) == object_info.end())
	    continue;	// skip clone base
	object_info[ptr.obj].live -= (limit - base);
	assert(object_info[ptr.obj].live >= 0);
	total_live_sectors -= (limit - base);
    }
    
    gc_sectors_written += data_sectors; // only written in this thread
    auto hdr = data_ptr0 - hdr_sectors*512;
    make_gc_hdr(hdr, _seq, data_sectors,
		obj_extents.data(), obj_extents.size());

    auto h = (obj_hdr*)hdr;
    assert(h->hdr_sectors == hdr_sectors);
    
    obj_info oi = {.hdr = hdr_sectors, .data = data_sectors,
		   .live = data_sectors};
    object_info[_seq] = oi;

    auto _p = (uint64_t *)(hdr + 512*hdr_sectors);
    assert(*_p != 0);
    
    objname name(prefix(_seq), _seq);
    auto req2 = objstore->make_write_req(name.c_str(), hdr,
					(hdr_sectors+data_sectors)*512);
    outstanding_writes++;
    req2->run(req);
}


void translate_impl::process_batch(int _seq, translate_req *req)
{
    req->_seq = _seq;
    
    int hdr_bytes = sizeof(obj_hdr) + sizeof(obj_data_hdr) +
	req->entries.size() * sizeof(data_map);
    int hdr_sectors = div_round_up(hdr_bytes, 512);
    char *hdr_ptr = req->data_ptr - hdr_sectors*512;
    int data_sectors = req->len/512;

    /* update the object info table
     */
    std::unique_lock obj_w_lock(*map_lock);
    obj_info oi = {.hdr = hdr_sectors, .data = data_sectors,
		   .live = data_sectors};
    object_info[_seq] = oi;

    /* and the object map (copy entries to right format at same time)
     */
    sector_t sector_offset = hdr_sectors;
    std::vector<extmap::lba2obj> deleted;
    std::vector<data_map> dm_entries;
    
    for (auto e : req->entries) {
	extmap::obj_offset oo = {_seq, sector_offset};
	map->update(e.lba, e.lba+e.len, oo, &deleted);
	sector_offset += e.len;
	dm_entries.push_back((data_map){(uint64_t)e.lba, (uint64_t)e.len});
    }

    for (auto d : deleted) {
	auto [base, limit, ptr] = d.vals();
	if (object_info.find(ptr.obj) == object_info.end())
	    continue;		// skip clone base
	object_info[ptr.obj].live -= (limit - base);
	assert(object_info[ptr.obj].live >= 0);
	total_live_sectors -= (limit - base);
    }

    /* update total and live sectors *after* dups are removed
     */
    auto live = object_info[_seq].live; 
    total_live_sectors += live;
    total_sectors += data_sectors; // doesn't count headers

    make_data_hdr(hdr_ptr, data_sectors*512, 0, &dm_entries, _seq, &uuid);
    
    objname name(prefix(_seq), _seq);
    auto req2 = objstore->make_write_req(name.c_str(), hdr_ptr,
					(hdr_sectors+data_sectors)*512);
    outstanding_writes++;
    req2->run(req);
}


void translate_impl::worker_thread(thread_pool<translate_req*> *p) {
    pthread_setname_np(pthread_self(), "batch_worker");
    while (p->running) {
	std::unique_lock lk(m);
	translate_req *req;
	if (!p->get_locked(lk, req)) 
           return;

	/* note - flush operation has to put any partial batch on the
	 * queue before queing a flush request
	 */
	if (req->op == REQ_FLUSH) {
	    while (outstanding_writes > 0)
		cv.wait(lk);
	    req->done = true;
	    req->cv.notify_all();
	}
	/* request and batch will be deleted on completion
	 * map is updated before any following requests are processed
	 */
	else if (req->op == REQ_PUT) {
	    auto _seq = seq++;
	    lk.unlock();
	    process_batch(_seq, req);
	}
	/* generate a checkpoint before any following requests processed
	 */
	else if (req->op == REQ_CKPT) {
	    auto _seq = seq++;
	    lk.unlock();
	    write_checkpoint(_seq, req);
	}
	else if (req->op == REQ_GC) {
	    auto _seq = seq++;
	    lk.unlock();
	    write_gc(_seq, req);
	}
	else
	    assert(false);
    }
}


/* flushes any data buffered in current batch, and blocks until all 
 * outstanding writes are complete.
 */
void translate_impl::flush(void) {
    std::unique_lock lk(m);
    
    if (current->len > 0) {
	workers->put_locked(current);
	current = new translate_req(REQ_PUT, cfg->batch_size, this);
    }

    auto flush_req = new translate_req(REQ_FLUSH, this);
    workers->put_locked(flush_req);
    lk.unlock();
    flush_req->wait();
}

void translate_impl::checkpoint(void) {
    std::unique_lock lk(m);
    
    if (current->len > 0) {
	workers->put_locked(current);
	current = new translate_req(REQ_PUT, cfg->batch_size, this);
    }

    auto ckpt_req = new translate_req(REQ_CKPT, this);
    workers->put_locked(ckpt_req);
    lk.unlock();
    ckpt_req->wait();
}


/* wake up every @wait_time, if data is pending in current batch
 * for @timeout then submit it for writing to the backend. 
 * Unlike flush() we don't bother waiting until it completes.
 */
void translate_impl::flush_thread(thread_pool<int> *p) {
    pthread_setname_np(pthread_self(), "flush_thread");
    auto wait_time = std::chrono::milliseconds(500);
    auto timeout = std::chrono::milliseconds(cfg->flush_msec);
    auto t0 = std::chrono::system_clock::now();
    auto seq0 = seq.load();

    while (p->running) {
	std::unique_lock lk(*p->m);
	p->cv.wait_for(lk, wait_time);
	if (!p->running)
	    break;

	if (p->running && seq0 == seq.load() && current->len > 0) {
	    if (std::chrono::system_clock::now() - t0 < timeout)
		continue;
	    workers->put_locked(current);
	    current = new translate_req(REQ_PUT, cfg->batch_size, this);
	}
	else {
	    seq0 = seq.load();
	    t0 = std::chrono::system_clock::now();
	}
    }
    printf("flush thread (%lx) exiting\n", pthread_self());
}


/* -------------- Garbage collection ---------------- */

struct _extent {
    int64_t base;
    int64_t limit;
    extmap::obj_offset ptr;
};

/* [describe GC algorithm here]
 */
void translate_impl::do_gc(bool *running) {
    gc_cycles++;
    int max_obj = seq.load();

    std::shared_lock obj_r_lock(*map_lock);
    std::vector<int> dead_objects;
    for (auto const &p : object_info)  {
	auto [hdrlen, datalen, live] = p.second;
	if (live == 0) {
	    total_sectors -= datalen;
	    dead_objects.push_back(p.first);
	}
    }
    obj_r_lock.unlock();

    std::queue<request*> deletes;
    for (auto const &o : dead_objects) {
	objname name(prefix(o), o);
	auto r = objstore->delete_object_req(name.c_str());
	r->run(NULL);
	deletes.push(r);
	while (deletes.size() > 8) {
	    deletes.front()->wait();
	    deletes.pop();
	}
    }
    while (deletes.size() > 0) {
	deletes.front()->wait();
	deletes.pop();
    }
    
    std::unique_lock obj_w_lock(*map_lock);
    for (auto const &o : dead_objects) 
	object_info.erase(o);
    obj_w_lock.unlock();
    
    /* create list of object info in increasing order of 
     * utilization, i.e. (live data) / (total size)
     */
    obj_r_lock.lock();
    int calculated_total = 0;
    std::set<std::tuple<double,int,int>> utilization;
    for (auto p : object_info)  {
	auto [hdrlen, datalen, live] = p.second;
	double rho = 1.0 * live / datalen;
	sector_t sectors = hdrlen + datalen;
	calculated_total += datalen;
	utilization.insert(std::make_tuple(rho, p.first, sectors));
	assert(sectors <= 20*1024*1024/512); // WTF??
    }
    obj_r_lock.unlock();
    
    /* gather list of objects needing cleaning, return if none
     */
    const double threshold = cfg->gc_threshold / 100.0;
    std::vector<std::pair<int,int>> objs_to_clean;
    for (auto [u, o, n] : utilization) {
	if (u > threshold)
	    continue;
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
    std::set<int> objects;
    for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++)
	objects.insert(it->first);

    int max_obj_sectors = 0;
    for (auto o : objects) {
	auto _sectors = object_info[o].hdr + object_info[o].data;
	max_obj_sectors = std::max(_sectors, max_obj_sectors);
    }
    
    obj_r_lock.lock();
    extmap::objmap live_extents;
    for (auto it = map->begin(); it != map->end(); it++) {
	auto [base, limit, ptr] = it->vals();
	if (ptr.obj <= max_obj && objects.find(ptr.obj) != objects.end())
	    live_extents.update(base, limit, ptr);
    }
    obj_r_lock.unlock();

    /* Retrieve objects to clean, save to local file, build map
     * TODO: 
     *  1. range read when appropriate
     *  2. get data from read cache when possible
     */
    if (live_extents.size() > 0) {
	/* temporary file, delete on close. 
	 */
	char temp[cfg->cache_dir.size() + 20];
	sprintf(temp, "%s/gc.XXXXXX", cfg->cache_dir.c_str());
	int fd = mkstemp(temp);

	/* read all objects in completely
	 */
	std::map<int,int> file_map; // obj# -> sector offset in file
	sector_t offset = 0;
	char *buf = (char*)malloc(max_obj_sectors*512);

	for (auto [i,sectors] : objs_to_clean) {
	    objname name(prefix(i), i);
	    objstore->read_object(name.c_str(), buf, sectors*512UL, 0);
	    gc_sectors_read += sectors;
	    file_map[i] = offset;
	    if (write(fd, buf, sectors*512) < 0)
		throw("no space");
	    offset += sectors;
	}
	free(buf);

	auto file_end = offset;

	std::vector<_extent> all_extents;
	for (auto it = live_extents.begin(); it != live_extents.end(); it++) {
	    auto [base, limit, ptr] = it->vals();
	    all_extents.push_back((_extent){base, limit, ptr});
	}
	
	/* outstanding writes
	 */
	std::queue<translate_req*> requests;
	
	while (all_extents.size() > 0) {
	    sector_t sectors = 0, max = cfg->batch_size / 512;
	    std::vector<_extent> extents;
	    
	    auto it = all_extents.begin();
	    while (it != all_extents.end() && sectors < max) {
		extents.push_back(*it);
		sectors += (it->limit - it->base);
		it = all_extents.erase(it);
	    }

	    auto req = new translate_req(REQ_GC, this);
	    auto file_data = req->gc_data = (char*)malloc(sectors * 512);
	    size_t file_data_len = 0;

	    for (auto [base, limit, ptr] : extents) {
		auto file_sector = file_map[ptr.obj] + ptr.offset;
		auto sectors = limit - base;
		assert(file_sector + sectors <= file_end);
		size_t bytes = sectors * 512;
		auto err = pread(fd, file_data + file_data_len, bytes,
				 file_sector*512);
		assert(err == (ssize_t)bytes);
		req->entries.push_back((ckpt_mapentry){
			.lba = base, .len = limit-base,
			    .obj = (int)ptr.obj, .offset = (int)ptr.offset});
		file_data_len += bytes;
	    }

	    std::unique_lock lk(m);
	    requests.push(req);
	    workers->put_locked(req);
	    lk.unlock();
	    
	    while (requests.size() > 8) {
		if (stopped)
		    return;
		auto t = requests.front();
		t->wait();
		requests.pop();
	    }
	}
	while (requests.size() > 0) {
	    if (stopped)
		return;
	    auto t = requests.front();
	    t->wait();
	    requests.pop();
	}
	close(fd);
	unlink(temp);
    }

    /* defer deletes until we've cleaned the whole batch. 
     */
    obj_w_lock.lock();
    for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++) {
	total_sectors -= object_info[it->first].data;
	object_info.erase(object_info.find(it->first));
    }
    obj_w_lock.unlock();
    
    if (stopped)
	return;

    /* write checkpoint *before* deleting any objects. 
     * use single-threaded delete for now
     */
    if (objs_to_clean.size()) {
	checkpoint();
	for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++) {
	    objname name(prefix(it->first), it->first);
	    objstore->delete_object(name.c_str());
	    gc_deleted++;
	}
    }
}

void translate_impl::stop_gc(void) {
    stopped = true;
    delete misc_threads;
    std::unique_lock lk(m);
    while (gc_running)
	gc_cv.wait(lk);
}

void translate_impl::gc_thread(thread_pool<int> *p) {
    auto interval = std::chrono::milliseconds(100);
    //sector_t trigger = 128 * 1024 * 2; // 128 MB
    const char *name = "gc_thread";
    pthread_setname_np(pthread_self(), name);
	
    while (p->running) {
	std::unique_lock lk(m);
	p->cv.wait_for(lk, interval);
	if (!p->running)
	    return;

	/* check to see if we should run a GC cycle
	 */
	//if (total_sectors - total_live_sectors < trigger)
	//    continue;
	//if ((total_live_sectors / (double)total_sectors) > (cfg->gc_threshold / 100.0))
	//continue;

	gc_running = true;
	lk.unlock();

	do_gc(&p->running);

	lk.lock();
	gc_running = false;
	gc_cv.notify_all();
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
	    objname name(prefix(obj), obj);
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

int translate_create_image(backend *objstore, const char *name,
			   uint64_t size) {
    char buf[4096];
    memset(buf, 0, 4096);

    auto _hdr = (obj_hdr*) buf;
    *_hdr = (obj_hdr){LSVD_MAGIC,
		      1,	  // version
		      {0},	  // UUID
		      LSVD_SUPER, // type
		      0,	  // seq
		      8,	  // hdr_sectors
		      0};	  // data_sectors
    uuid_generate_random(_hdr->vol_uuid);

    auto _super = (super_hdr*)(_hdr + 1);
    uint64_t sectors = size / 512;
    *_super = (super_hdr){sectors, // vol_size
			  0, 0,	   // checkpoint offset, len
			  0, 0,	   // clone offset, len
			  0, 0};   // snap offset, len
    
    auto rv = objstore->write_object(name, buf, 4096);
    return rv;
}

int translate_get_uuid(backend *objstore, const char *name, uuid_t &uu) {
    char buf[4096];
    int rv = objstore->read_object(name, buf, sizeof(buf), 0);
    if (rv < 0)
	return rv;
    auto hdr = (obj_hdr*)buf;
    memcpy(uu, hdr->vol_uuid, sizeof(uuid_t));
    return 0;
}

int translate_remove_image(backend *objstore, const char *name) {

    /* read the superblock to get the list of checkpoints
     */
    char buf[4096];
    int rv = objstore->read_object(name, buf, sizeof(buf), 0);
    if (rv < 0)
	return rv;
    auto hdr = (obj_hdr*) buf;
    auto sh = (super_hdr*)(hdr+1);

    if (hdr->magic != LSVD_MAGIC || hdr->type != LSVD_SUPER)
	return -1;

    int seq = 1;
    std::vector<uint32_t> ckpts;
    decode_offset_len<uint32_t>(buf, sh->ckpts_offset, sh->ckpts_len, ckpts);

    /* read the most recent checkpoint and get its object map
     */
    if (ckpts.size() > 0) {
	object_reader r(objstore);
	seq = ckpts.back();
	objname obj(name, seq);
	auto ckpt_buf = r.read_object_hdr(obj.c_str(), false);
	auto c_hdr = (obj_hdr*)ckpt_buf;
	auto c_data = (obj_ckpt_hdr*)(c_hdr+1);
	if (c_hdr->magic != LSVD_MAGIC || c_hdr->type != LSVD_CKPT)
	    return -1;
	std::vector<ckpt_obj> objects;
	decode_offset_len<ckpt_obj>(ckpt_buf, c_data->objs_offset,
				    c_data->objs_len, objects);

	/* delete all the objects in the objmap
	 */
	std::queue<request*> deletes;
	for (auto const & o : objects) {
	    objname obj(name, o.seq);
	    auto r = objstore->delete_object_req(obj.c_str());
	    r->run(NULL);
	    deletes.push(r);
	    while (deletes.size() > 8) {
		deletes.front()->wait();
		deletes.pop();
	    }
	}
	while (deletes.size() > 0) {
	    deletes.front()->wait();
	    deletes.pop();
	}
	
	/* delete all the checkpoints
	 */
	for (auto const & c : ckpts) {
	    objname obj(name, c);
	    objstore->delete_object(obj.c_str());
	}
	free(ckpt_buf);
    }
    /* delete any objects after the last checkpoint, up to the first run of
     * 32 missing sequence numbers
     */
    for (int n = 0; n < 16; seq++, n++) {
	objname obj(name, seq);
	if (objstore->delete_object(obj.c_str()) >= 0)
	    n = 0;
    }

    /* and delete the superblock
     */
    objstore->delete_object(name);
    return 0;
}

