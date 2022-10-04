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

#include <uuid/uuid.h>

#include <vector>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <atomic>

#include <stack>
#include <map>

#include <algorithm>

#include "base_functions.h"
#include <thread>
#include "smartiov.h"
#include "extent.h"

#include "backend.h"
#include "objects.h"
#include "misc_cache.h"
#include "translate.h"
#include "objname.h"

/* TODO: MAKE THESE INSTANCE VARIABLES */
extern int batch_seq;
extern int last_ckpt;
extern uuid_t my_uuid;
class batch;
template class thread_pool<batch*>;
template class thread_pool<int>;

/* batch: a bunch of writes being accumulated, waiting to be
 * written to the backend. 
 */
class batch {
public:
    char  *buf;			// data goes here
    size_t len;			// current data size
    size_t max;			// done when len hits here
    std::vector<data_map> entries;
    int    seq;			// sequence number for backend

    batch(size_t _max) {
        buf = (char*)malloc(_max);
        max = _max;
    }
    ~batch() {
        free((void*)buf);
    }

    void reset(void);
    void append_iov(uint64_t lba, iovec *iov, int iovcnt);
    int hdrlen(void);
};

/* recycle a batch object so we can use it again.
 */
void batch::reset(void) {
    len = 0;
    entries.resize(0);
    seq = batch_seq++;		// TODO: FIX ME PLEASE
}

/* add a write to the batch. Copies it into the buffer and adds 
 * an extent entry to entries[] 
 */
void batch::append_iov(uint64_t lba, iovec *iov, int iovcnt) {
    char *ptr = buf + len;
    for (int i = 0; i < iovcnt; i++) {
	memcpy(ptr, iov[i].iov_base, iov[i].iov_len);
	entries.push_back((data_map){lba, iov[i].iov_len / 512});
	ptr += iov[i].iov_len;
	len += iov[i].iov_len;
	lba += iov[i].iov_len / 512;
    }
}

/* How many bytes will we need. 
 * TODO: FIX ME - the 'sizeof(hdr) + sizeof(data_hdr)' is gross
 */
int batch::hdrlen(void) {
    return sizeof(hdr) + sizeof(data_hdr) + entries.size() * sizeof(data_map);
}

/* ----------- Object translation layer -------------- */

class translate_impl : public translate {
    std::mutex   m;
    objmap      *map; /* map shared between read cache and translate */
    
    batch              *current_batch = NULL;
    std::stack<batch*>  batches;
    std::map<int,char*> in_mem_objects;

    /* info on all live objects - all sizes in sectors */
    struct obj_info {
	uint32_t hdr;
	uint32_t data;
	uint32_t live;
	int      type;
    };
    std::map<int,obj_info> object_info;

    char      *single_prefix;

    char      *super_name;
    char      *super_buf = NULL;
    hdr       *super_h = NULL;
    super_hdr *super_sh = NULL;
    size_t     super_len;

    std::atomic<int> puts_outstanding = 0;
    
    thread_pool<batch*> workers;
    thread_pool<int>    misc_threads;

    /* for flush()
     */
    int last_written = 0;
    int last_pushed = 0;
    std::condition_variable cv;

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

    /* completions may come in out of order; need to update the map in 
     * order of sequence numbers. 
     */
    std::set<std::pair<int32_t,void*>> completions;
    int32_t                            next_completion = 0;
    std::mutex                         m_c;
    std::condition_variable            cv_c;

    void do_completions(int32_t seq, void *closure);

    int write_checkpoint(int seq);

    int make_hdr(char *buf, batch *b);
    sector_t make_gc_hdr(char *buf, uint32_t seq, sector_t sectors,
			 data_map *extents, int n_extents);

    void do_gc(std::unique_lock<std::mutex> &lk);
    void gc_thread(thread_pool<int> *p);
    void worker_thread(thread_pool<batch*> *p);
    void ckpt_thread(thread_pool<int> *p);
    void flush_thread(thread_pool<int> *p);

    backend *objstore;
    
public:
    translate_impl(backend *_io, objmap *omap);
    ~translate_impl();

    ssize_t init(const char *name, int nthreads, bool timedflush);
    void shutdown(void);

    int flush(void);            /* write out current batch */
    int checkpoint(void);       /* flush, then write checkpoint */

    ssize_t writev(size_t offset, iovec *iov, int iovcnt);
    ssize_t readv(size_t offset, iovec *iov, int iovcnt);

    const char *prefix() { return single_prefix; }
    
    /* debug functions
     */
    void getmap(int base, int limit,
                int (*cb)(void *ptr,int,int,int,int), void *ptr);
    int mapsize(void);
    void reset(void);
    int frontier(void);
};

translate_impl::translate_impl(backend *_io, objmap *omap) :
    workers(&m), misc_threads(&m) {
    objstore = _io;
    parser = new object_reader(objstore);
    map = omap;
    current_batch = NULL;
    last_written = last_pushed = 0;
}

translate *make_translate(backend *_io, objmap *omap) {
    return (translate*) new translate_impl(_io, omap);
}

translate_impl::~translate_impl() {
    while (!batches.empty()) {
	auto b = batches.top();
	batches.pop();
	delete b;
    }
    if (current_batch)
	delete current_batch;
    delete parser;
    if (super_buf)
	free(super_buf);
    free(single_prefix);
    free(super_name);
}

ssize_t translate_impl::init(const char *prefix_,
			     int nthreads, bool timedflush) {
    std::vector<uint32_t>    ckpts;
    std::vector<clone_info*> clones;
    std::vector<snap_info>   snaps;

    /* note prefix = superblock name
     */
    super_name = strdup(prefix_);
    single_prefix = strdup(prefix_);

    auto [_buf, bytes] = parser->read_super(super_name, ckpts, clones,
					    snaps, my_uuid);
    if (bytes < 0)
	return bytes;
    super_buf = _buf;
    super_h = (hdr*)super_buf;
    super_len = super_h->hdr_sectors * 512;
    super_sh = (super_hdr*)(super_h+1);
    
    batch_seq = super_sh->next_obj;

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
	    object_info[o.seq] = (obj_info){.hdr = o.hdr_sectors,
					    .data = o.data_sectors,
					    .live = o.live_sectors,
					    .type = LSVD_DATA};
	    total_sectors += o.data_sectors;
	    total_live_sectors += o.live_sectors;
	}
	for (auto m : entries) {
	    map->map.update(m.lba, m.lba + m.len,
			    (extmap::obj_offset){.obj = m.obj,
				    .offset = m.offset});
	}
	_ckpt = ck;
    }

    for (int i = _ckpt; ; i++) {
	std::vector<uint32_t>    ckpts;
	std::vector<obj_cleaned> cleaned;
	std::vector<data_map>    entries;
	hdr h; data_hdr dh;
	batch_seq = i;
	objname name(prefix(), i);
	if (parser->read_data_hdr(name.c_str(), h, dh, ckpts,
				  cleaned, entries) < 0)
	    break;
	object_info[i] = (obj_info){.hdr = h.hdr_sectors, .data = h.data_sectors,
				    .live = h.data_sectors, .type = LSVD_DATA};
	total_sectors += h.data_sectors;
	total_live_sectors += h.data_sectors;
	int offset = 0, hdr_len = h.hdr_sectors;
	for (auto m : entries) {
	    std::vector<extmap::lba2obj> deleted;
	    extmap::obj_offset oo = {i, offset + hdr_len};
	    map->map.update(m.lba, m.lba + m.len, oo, &deleted);
	    offset += m.len;
	    for (auto d : deleted) {
		auto [base, limit, ptr] = d.vals();
		object_info[ptr.obj].live -= (limit - base);
		total_live_sectors -= (limit - base);
	    }
	}
    }
    next_completion = batch_seq;

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


/* create header for a data object
 */
int translate_impl::make_hdr(char *buf, batch *b) {
    hdr *h = (hdr*)buf;
    data_hdr *dh = (data_hdr*)(h+1);
    uint32_t o1 = sizeof(*h) + sizeof(*dh), l1 = sizeof(uint32_t),
	o2 = o1 + l1, l2 = b->entries.size() * sizeof(data_map),
	hdr_bytes = o2 + l2;
    sector_t hdr_sectors = div_round_up(hdr_bytes, 512);
	
    *h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
	       .type = LSVD_DATA, .seq = (uint32_t)b->seq,
	       .hdr_sectors = (uint32_t)hdr_sectors,
	       .data_sectors = (uint32_t)(b->len / 512)};
    memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));

    *dh = (data_hdr){.last_data_obj = (uint32_t)b->seq, .ckpts_offset = o1,
		     .ckpts_len = l1,
		     .objs_cleaned_offset = 0, .objs_cleaned_len = 0,
		     .map_offset = o2, .map_len = l2};

    uint32_t *p_ckpt = (uint32_t*)(dh+1);
    *p_ckpt = last_ckpt;

    data_map *dm = (data_map*)(p_ckpt+1);
    for (auto e : b->entries)
	*dm++ = e;

    return (char*)dm - (char*)buf;
}

/* create header for a GC object
 */
sector_t translate_impl::make_gc_hdr(char *buf, uint32_t seq, sector_t sectors,
				     data_map *extents, int n_extents) {
    hdr *h = (hdr*)buf;
    data_hdr *dh = (data_hdr*)(h+1);
    uint32_t o1 = sizeof(*h) + sizeof(*dh), l1 = sizeof(uint32_t),
	o2 = o1 + l1, l2 = n_extents * sizeof(data_map),
	hdr_bytes = o2 + l2;
    sector_t hdr_sectors = div_round_up(hdr_bytes, 512);

    *h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
	       .type = LSVD_DATA, .seq = seq,
	       .hdr_sectors = (uint32_t)hdr_sectors,
	       .data_sectors = (uint32_t)sectors};
    memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));

    *dh = (data_hdr){.last_data_obj = seq, .ckpts_offset = o1, .ckpts_len = l1,
		     .objs_cleaned_offset = 0, .objs_cleaned_len = 0,
		     .map_offset = o2, .map_len = l2};

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

/* TODO: 
 * if the read logic gets handled in what's currently the read cache,
 * then there's no need to handle reads from memory. 
 * - enter extents into map once, when writing batch
 * - update unit tests to use flush() when testing translation layer
 * - get rid of all that completion processing nonsense
 */

/* an awful lot of code to copy data to the current batch and then
 * toss it to the worker thread pool if it's full
 * NOTE: offset is in bytes
 */
ssize_t translate_impl::writev(size_t offset, iovec *iov, int iovcnt) {
    std::unique_lock<std::mutex> lk(m);
    size_t len = iov_sum(iov, iovcnt);

    while (puts_outstanding >= 32)
	cv.wait(lk);
	
    if (current_batch && current_batch->len + len > current_batch->max) {
	last_pushed = current_batch->seq;
	workers.put_locked(current_batch);
	current_batch = NULL;
    }
    if (current_batch == NULL) {
	if (batches.empty()) {
	    current_batch = new batch(BATCH_SIZE);
	}
	else {
	    current_batch = batches.top();
	    batches.pop();
	}
	current_batch->reset();
	in_mem_objects[current_batch->seq] = current_batch->buf;
    }

    int64_t sector_offset = current_batch->len / 512,
	lba = offset/512, limit = (offset+len)/512;
    current_batch->append_iov(offset / 512, iov, iovcnt);

    /* TODO: isn't the map updated *after* this the batch is written?
     * maybe not - need to figure out
     */
    std::vector<extmap::lba2obj> deleted;
    extmap::obj_offset oo = {current_batch->seq, sector_offset};
    std::unique_lock objlock(map->m);

    map->map.update(lba, limit, oo, &deleted);
    for (auto d : deleted) {
	auto [base, limit, ptr] = d.vals();
	object_info[ptr.obj].live -= (limit - base);
	total_live_sectors -= (limit - base);
    }
	
    return len;
}

/* worker: pull batches from queue and write them out
 */
void translate_impl::worker_thread(thread_pool<batch*> *p) {
    while (p->running) {
	std::unique_lock<std::mutex> lk(m);
	batch *b;
	if (!p->get_locked(lk, b))
	    return;
	uint32_t hdr_sectors = div_round_up(b->hdrlen(), 512);
	object_info[b->seq] = (obj_info){.hdr = hdr_sectors, .data = (uint32_t)(b->len/512),
					 .live = (uint32_t)(b->len/512), .type = LSVD_DATA};
	total_sectors += b->len/512;
	total_live_sectors += b->len/512;
	lk.unlock();

	char *hdr = (char*)calloc(hdr_sectors*512, 1);
	make_hdr(hdr, b);
	auto iovs = new smartiov;
	iovs->push_back((iovec){hdr, (size_t)(hdr_sectors*512)});
	iovs->push_back((iovec){b->buf, b->len});

	/* Note that we've already decremented object live counts when 
	 * we copied the data into the batch buffer.
	 */
	auto closure = wrap([this, hdr_sectors, hdr, iovs, b]{
		std::unique_lock lk(m);
		std::unique_lock objlock(map->m);

		auto offset = hdr_sectors;
		for (auto e : b->entries) {
		    extmap::obj_offset oo = {b->seq, offset};
		    map->map.update(e.lba, e.lba+e.len, oo, NULL);
		    offset += e.len;
		}

		last_written = std::max(last_written, b->seq); // for flush()
		cv.notify_all();
	    
		in_mem_objects.erase(b->seq);
		batches.push(b);
		objlock.unlock();
		free(hdr);
		delete iovs;
		puts_outstanding--;
		cv.notify_all();
		return true;
	    });
	auto closure2 = wrap([this, closure, b]{
		do_completions(b->seq, closure);
		return true;
	    });
	puts_outstanding++;

	objname name(prefix(), b->seq);
	auto cb_req = new callback_req(call_wrapped, closure2);
	auto req = objstore->make_write_req(name.c_str(), iovs->data(),
					    iovs->size());
	req->run(cb_req);
    }
}

/* need to apply map updates in sequence
 * completions is std::set of std::pair<seq#,closure>, ordered by seq#
 * next_completion steps through sequence numbers
 *
 * note that we use a separate mutex for the completion queue so 
 * that we can safely hold it through the callback
 */
void translate_impl::do_completions(int32_t seq, void *closure) {
    std::unique_lock lk(m_c);
    if (seq == next_completion) { /* am I next? */
	next_completion++;
	call_wrapped(closure);	/* if so, just handle it */
    }
    else
	completions.emplace(seq, closure); /* otherwise queue me */

    /* process any more in-order completions
     */
    auto it = completions.begin();
    while (it != completions.end() && it->first == next_completion) {
	call_wrapped(it->second);
	it = completions.erase(it);
	next_completion++;
    }
    lk.unlock();
    cv_c.notify_all();
}

int translate_impl::flush(void) {
    std::unique_lock<std::mutex> lk(m);
    int val = 0;
    if (current_batch && current_batch->len > 0) {
	val = current_batch->seq;
	last_pushed = val;
	workers.put_locked(current_batch);
	current_batch = NULL;
    }
    while (last_written < last_pushed)
	cv.wait(lk);
    return val;
}

void translate_impl::flush_thread(thread_pool<int> *p) {
    auto wait_time = std::chrono::milliseconds(500);
    auto timeout = std::chrono::seconds(2);
    auto t0 = std::chrono::system_clock::now();
    auto seq0 = batch_seq;

    while (p->running) {
	std::unique_lock<std::mutex> lk(*p->m);
	p->cv.wait_for(lk, wait_time);
	if (p->running && current_batch && seq0 == batch_seq &&
	    current_batch->len > 0) {
	    if (std::chrono::system_clock::now() - t0 > timeout) {
		lk.unlock();
		flush();
	    }
	}
	else {
	    seq0 = batch_seq;
	    t0 = std::chrono::system_clock::now();
	}
    }
}

/* synchronous read from offset
 * NOTE: offset is in bytes
 */
ssize_t translate_impl::readv(size_t offset, iovec *iov, int iovcnt) {
    smartiov iovs(iov, iovcnt);
    size_t len = iovs.bytes();
    int64_t base = offset / 512;
    int64_t sectors = len / 512, limit = base + sectors;
	
    if (map->map.size() == 0) {
	iovs.zero();
	return len;
    }

    /* object number, offset (bytes), length (bytes) */
    std::vector<std::tuple<int, size_t, size_t>> regions;

    std::unique_lock lk(m);
    std::shared_lock slk(map->m);
	
    auto prev = base;
    int iov_offset = 0;

    for (auto it = map->map.lookup(base);
	 it != map->map.end() && it->base() < limit; it++) {
	auto [_base, _limit, oo] = it->vals(base, limit);
	if (_base > prev) {	// unmapped
	    size_t _len = (_base - prev)*512;
	    regions.push_back(std::tuple(-1, 0, _len));
	    iov_offset += _len;
	}
	size_t _len = (_limit - _base) * 512,
	    _offset = oo.offset * 512;
	int obj = oo.obj;
	if (in_mem_objects.find(obj) != in_mem_objects.end()) {
	    iovs.slice(iov_offset, iov_offset+_len).copy_in(
		in_mem_objects[obj]+_offset);
	    obj = -2;
	}
	regions.push_back(std::tuple(obj, _offset, _len));
	iov_offset += _len;
	prev = _limit;
    }
    slk.unlock();
    lk.unlock();

    iov_offset = 0;
    for (auto [obj, _offset, _len] : regions) {
	auto slice = iovs.slice(iov_offset, iov_offset + _len);
	if (obj == -1)
	    slice.zero();
	else if (obj == -2)
	    /* skip */;
	else {
	    objname name(prefix(), obj);
	    auto [iov,iovcnt] = slice.c_iov();
	    objstore->read_object(name.c_str(), iov, iovcnt, _offset);
	}
	iov_offset += _len;
    }

    return iov_offset;
}

/* -------------- Checkpointing -------------- */

/* synchronously write a checkpoint
 */
int translate_impl::write_checkpoint(int seq) {
    std::vector<ckpt_mapentry> entries;
    std::vector<ckpt_obj> objects;

    /* hold the map lock while we get a copy of the map
     */
    std::unique_lock lk(map->m);
    last_ckpt = seq;
    for (auto it = map->map.begin(); it != map->map.end(); it++) {
	auto [base, limit, ptr] = it->vals();
	entries.push_back((ckpt_mapentry){.lba = base, .len = limit-base,
		    .obj = (int32_t)ptr.obj, .offset = (int32_t)ptr.offset});
    }
    lk.unlock();
    size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);

    /* hold the translation layer lock while we get a copy of object_info
     */
    std::unique_lock lk2(m);
    for (auto it = object_info.begin(); it != object_info.end(); it++) {
	auto obj_num = it->first;
	auto [hdr, data, live, type] = it->second;
	if (type == LSVD_DATA)
	    objects.push_back((ckpt_obj){.seq = (uint32_t)obj_num, .hdr_sectors = hdr,
			.data_sectors = data, .live_sectors = live});
    }

    /* add object for this checkpoint
     */
    size_t objs_bytes = objects.size() * sizeof(ckpt_obj);
    size_t hdr_bytes = sizeof(hdr) + sizeof(ckpt_hdr);
    uint32_t sectors = div_round_up(hdr_bytes + sizeof(seq) + map_bytes +
				    objs_bytes, 512);
    object_info[seq] = (obj_info){.hdr = sectors, .data = 0, .live = 0,
				  .type = LSVD_CKPT};
    lk2.unlock();

    /* put it all together in memory
     */
    char *buf = (char*)calloc(hdr_bytes, 1);
    hdr *h = (hdr*)buf;
    *h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
	       .type = LSVD_CKPT, .seq = (uint32_t)seq,
	       .hdr_sectors = sectors, .data_sectors = 0};
    memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));
    ckpt_hdr *ch = (ckpt_hdr*)(h+1);

    uint32_t o1 = sizeof(hdr)+sizeof(ckpt_hdr), o2 = o1 + sizeof(seq),
	o3 = o2 + objs_bytes;
    *ch = (ckpt_hdr){.ckpts_offset = o1, .ckpts_len = sizeof(seq),
		     .objs_offset = o2, .objs_len = o3-o2,
		     .deletes_offset = 0, .deletes_len = 0,
		     .map_offset = o3, .map_len = (uint32_t)map_bytes};

    iovec iov[] = {{.iov_base = buf, .iov_len = hdr_bytes},
		   {.iov_base = (char*)&seq, .iov_len = sizeof(seq)},
		   {.iov_base = (char*)objects.data(), objs_bytes},
		   {.iov_base = (char*)entries.data(), map_bytes}};

    /* wait until all objects up through seq-1 have been successfully
     * written, then write it
     */
    {
	std::unique_lock lk_complete(m_c);
	while (next_completion < seq)
	    cv_c.wait(lk_complete);
	next_completion++;
    }

    objname name(prefix(), seq);
    objstore->write_object(name.c_str(), iov, 4);
    free(buf);

    /* Now re-write the superblock with the new list of checkpoints
     */
    size_t offset = sizeof(*super_h) + sizeof(*super_sh);

    // lk2.lock(); TODO: WTF???
    super_sh->ckpts_offset = offset;
    super_sh->ckpts_len = sizeof(seq);
    *(int*)(super_buf + offset) = seq;
    struct iovec iov2 = {super_buf, 4096};
    objstore->write_object(super_name, &iov2, 1);
    return seq;
}

void translate_impl::ckpt_thread(thread_pool<int> *p) {
    auto one_second = std::chrono::seconds(1);
    auto seq0 = batch_seq;
    const int ckpt_interval = 100;

    while (p->running) {
	std::unique_lock<std::mutex> lk(m);
	p->cv.wait_for(lk, one_second);
	if (p->running && batch_seq - seq0 > ckpt_interval) {
	    seq0 = batch_seq;
	    lk.unlock();
	    checkpoint();
	}
    }
}

int translate_impl::checkpoint(void) {
    std::unique_lock<std::mutex> lk(m);
    if (current_batch && current_batch->len > 0) {
	last_pushed = current_batch->seq;
	workers.put_locked(current_batch);
	current_batch = NULL;
    }
    if (map->map.size() == 0)
	return 0;
    int seq = batch_seq++;
    lk.unlock();
    return write_checkpoint(seq);
}


/* -------------- Garbage collection ---------------- */

/* Needs a lot of work. Note that all reads/writes are synchronous
 * for now...
 */
void translate_impl::do_gc(std::unique_lock<std::mutex> &lk) {
    assert(!m.try_lock());	// must be locked
    gc_cycles++;
    int max_obj = batch_seq;

    /* create list of object info in increasing order of 
     * utilization, i.e. (live data) / (total size)
     */
    std::set<std::tuple<double,int,int>> utilization;
    for (auto p : object_info)  {
	if (in_mem_objects.find(p.first) != in_mem_objects.end())
	    continue;
	double rho = 1.0 * p.second.live / p.second.data;
	sector_t sectors = p.second.hdr + p.second.data;
	utilization.insert(std::make_tuple(rho, p.first, sectors));
    }

    /* gather list of objects needing cleaning, return if none
     */
    const double threshold = 0.55;
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
	
    /* find all live extents in objects listed in objs_to_clean
     */
    std::vector<bool> bitmap(max_obj+1);
    for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++)
	bitmap[it->first] = true;

    /* TODO: I think we need to hold the objmap lock here...
     */
    extmap::objmap live_extents;
    for (auto it = map->map.begin(); it != map->map.end(); it++) {
	auto [base, limit, ptr] = it->vals();
	if (bitmap[ptr.obj])
	    live_extents.update(base, limit, ptr);
    }
    lk.unlock();

    /* everything before this point was in-memory only, with the 
     * translation instance mutex held, doing no I/O.
     */

    /* this is really gross. We really should check to see if data is
     * in the read cache, and put retrieved data there...
     */
    if (live_extents.size() > 0) {
	/* temporary file, delete on close. Should use mkstemp().
	 * need a proper config file so we can configure all this crap...
	 */
	char temp[] = "/mnt/nvme/lsvd/gc.XXXXXX";
	int fd = mkstemp(temp);
	unlink(temp);

	/* read all objects in completely. Someday we can check to see whether
	 * (a) data is already in cache, or (b) sparse reading would be quicker
	 */
	extmap::cachemap file_map;
	sector_t offset = 0;
	//char *buf = (char*)malloc(10*1024*1024);
	char *buf = (char*)aligned_alloc(512, 10*1024*1024); // TODO: WHY??

	for (auto [i,sectors] : objs_to_clean) {
	    objname name(prefix(), i);
	    iovec iov = {buf, (size_t)(sectors*512)};
	    objstore->read_object(name.c_str(), &iov, 1, /*offset=*/ 0);
	    gc_sectors_read += sectors;
	    extmap::obj_offset _base = {i, 0}, _limit = {i, sectors};
	    file_map.update(_base, _limit, offset);
	    write(fd, buf, sectors*512);
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
	//live_extents.reset();	// give up memory early
	//printf("\ngc: to clean: %ld extents\n", all_extents.size());
	
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

	    lk.lock();
	    off_t byte_offset = 0;
	    sector_t data_sectors = 0;
	    auto obj_extents = new std::vector<data_map>();
	    
	    for (auto [base, limit, ptr] : extents) {
		for (auto it2 = map->map.lookup(base);
		     it2->base() < limit && it2 != map->map.end(); it2++) {
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
		    obj_extents->push_back((data_map){(uint64_t)_base, (uint64_t)_sectors});

		    data_sectors += _sectors;
		    byte_offset += bytes;
		}
	    }
	    int32_t seq = batch_seq++;
	    lk.unlock();

	    gc_sectors_written += data_sectors;
	    
	    int hdr_sectors = make_gc_hdr(hdr, seq, data_sectors,
					  obj_extents->data(), obj_extents->size());
	    auto iovs = new smartiov;
	    iovs->push_back((iovec){hdr, (size_t)hdr_sectors*512});
	    iovs->push_back((iovec){buf, (size_t)byte_offset});

	    auto closure = wrap([this, hdr_sectors, obj_extents, buf, hdr, seq, iovs]{
		    std::unique_lock lk(m);
		    std::unique_lock objlock(map->m);
		    auto offset = hdr_sectors;
		    for (auto e : *obj_extents) {
			extmap::obj_offset oo = {seq, offset};
			map->map.update(e.lba, e.lba+e.len, oo, NULL);
			offset += e.len;
		    }

		    // is this (last_written) needed?
		    last_written = std::max(last_written, seq); 
		    cv.notify_all();

		    delete obj_extents;
		    delete iovs;
		    free(buf);
		    free(hdr);
		    return true;
		});
	    auto closure2 = wrap([this, closure, seq]{
		    do_completions(seq, closure);
		    return true;
		});
	    objname name(prefix(), seq);
	    auto cb_req = new callback_req(call_wrapped, closure2);
	    auto [iov,iovcnt] = iovs->c_iov();
	    auto req = objstore->make_write_req(name.c_str(), iov, iovcnt);
	    req->run(cb_req);
	}
	close(fd);
    }
	
    for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++) {
	objname name(prefix(), it->first);
	objstore->delete_object(name.c_str());
	gc_deleted++;
    }
    lk.lock();
}


void translate_impl::gc_thread(thread_pool<int> *p) {
    auto interval = std::chrono::milliseconds(100);
    sector_t trigger = 128 * 1024 * 2; // 128 MB
    const char *name = "gc";
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
    


/* debug methods
 */
void translate_impl::getmap(int base, int limit,
		       int (*cb)(void *ptr,int,int,int,int), void *ptr) {
    for (auto it = map->map.lookup(base); it != map->map.end() && it->base() < limit; it++) {
	auto [_base, _limit, oo] = it->vals(base, limit);
	if (!cb(ptr, (int)_base, (int)_limit, (int)oo.obj, (int)oo.offset))
	    break;
    }
}
int translate_impl::mapsize(void) {
    return map->map.size();
}
void translate_impl::reset(void) {
    map->map.reset();
}
int translate_impl::frontier(void) {
    if (current_batch)
	return current_batch->len / 512;
    return 0;
}

