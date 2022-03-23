/*
 * file:        lsvd_rbd.cc
 * description: userspace block-on-object layer with librbd interface
 * 
 * Copyright 2021, 2022 Peter Desnoyers
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "extent.cc"
#include "objects.cc"
#include "journal2.cc"

#include <vector>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <stack>
#include <map>
#include <thread>
#include <ios>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <chrono>
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#include <random>
#include <algorithm>
#include <list>
#include <atomic>

#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

std::mutex printf_m;
#define xprintf(...) do { \
	std::unique_lock lk(printf_m); \
	fprintf(stderr, __VA_ARGS__); \
    } while (0)

struct _log {
    int l;
    pthread_t th;
    long arg;
} *logbuf, *logptr;

void dbg(int l, long arg)
{
    logptr->l = l;
    logptr->th = pthread_self();
    logptr->arg = arg;
    logptr++;
}
//#define DBG(a) dbg(__LINE__, a)
#define DBG(a) 

void printlog(FILE *fp)
{
    while (logbuf != logptr) {
	fprintf(fp, "%d %lx %lx\n", logbuf->l, logbuf->th, logbuf->arg);
	logbuf++;
    }
}

// https://stackoverflow.com/questions/5008804/generating-random-integer-from-a-range
std::random_device rd;     // only used once to initialise (seed) engine
//std::mt19937 rng(rd());  // random-number engine used (Mersenne-Twister in this case)
std::mt19937 rng(17);      // for deterministic testing

typedef int64_t lba_t;
typedef int64_t sector_t;

/* make this atomic? */
int batch_seq;
int last_ckpt;
const int BATCH_SIZE = 8 * 1024 * 1024;
uuid_t my_uuid;

static int div_round_up(int n, int m)
{
    return (n + m - 1) / m;
}
static int round_up(int n, int m)
{
    return m * div_round_up(n, m);
}

size_t iov_sum(const iovec *iov, int iovcnt)
{
    size_t sum = 0;
    for (int i = 0; i < iovcnt; i++)
	sum += iov[i].iov_len;
    return sum;
}

std::string hex(uint32_t n)
{
    std::stringstream stream;
    stream << std::setfill ('0') << std::setw(8) << std::hex << n;
    return stream.str();
}

std::mutex   m; 		// for now everything uses one mutex

class backend {
public:
    virtual ssize_t write_object(const char *name, iovec *iov, int iovcnt) = 0;
    virtual ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) = 0;
    virtual ssize_t read_object(const char *name, char *buf, size_t offset, size_t len) = 0;
    virtual ssize_t read_numbered_object(int seq, char *buf, size_t offset, size_t len) = 0;
    virtual std::string object_name(int seq) = 0;
    virtual ~backend(){}
};

struct batch {
    char  *buf;
    size_t max;
    size_t len;
    int    seq;
    std::vector<data_map> entries;
public:
    batch(size_t _max) {
	buf = (char*)malloc(_max);
	max = _max;
    }
    ~batch(){
	free((void*)buf);
    }
    void reset(void) {
	len = 0;
	entries.resize(0);
	seq = batch_seq++;
    }
    void append_iov(uint64_t lba, iovec *iov, int iovcnt) {
	char *ptr = buf + len;
	for (int i = 0; i < iovcnt; i++) {
	    memcpy(ptr, iov[i].iov_base, iov[i].iov_len);
	    entries.push_back((data_map){lba, iov[i].iov_len / 512});
	    ptr += iov[i].iov_len;
	    len += iov[i].iov_len;
	    lba += iov[i].iov_len / 512;
	}
    }
    int hdrlen(void) {
	return sizeof(hdr) + sizeof(data_hdr) + entries.size() * sizeof(data_map);
    }
};

template <class T>
class thread_pool {
public:
    std::queue<T> q;
    bool         running;
    std::mutex  *m;
    std::condition_variable cv;
    std::queue<std::thread> pool;
    
    thread_pool(std::mutex *_m) {
	running = true;
	m = _m;
    }
    ~thread_pool() {
	std::unique_lock lk(*m);
	running = false;
	cv.notify_all();
	lk.unlock();
	while (!pool.empty()) {
	    pool.front().join();
	    pool.pop();
	}
    }	
    bool get_locked(std::unique_lock<std::mutex> &lk, T &val) {
	while (running && q.empty())
	    cv.wait(lk);
	if (!running)
	    return false;
	val = q.front();
	q.pop();
	return val;
    }
    bool get(T &val) {
	std::unique_lock<std::mutex> lk(*m);
	return get_locked(lk, val);
    }
    bool wait_locked(std::unique_lock<std::mutex> &lk) {
	while (running && q.empty())
	    cv.wait(lk);
	return running;
    }
    bool get_nowait(T &val) {
	if (!running || q.empty())
	    return false;
	val = q.front();
	q.pop();
	return true;
    }
    void put_locked(T work) {
	q.push(work);
	cv.notify_one();
    }
    void put(T work) {
	std::unique_lock<std::mutex> lk(*m);
	put_locked(work);
    }
};

/* these all should probably be combined with the stuff in objects.cc to create
 * object classes that serialize and de-serialize themselves. Sometime, maybe.
 */
template <class T>
void decode_offset_len(char *buf, size_t offset, size_t len, std::vector<T> &vals) {
    T *p = (T*)(buf + offset), *end = (T*)(buf + offset + len);
    for (; p < end; p++)
	vals.push_back(*p);
}


class objmap {
public:
    std::shared_mutex m;
    extmap::objmap    map;
};


class translate {
    // mutex protects batch, thread pools, object_info, in_mem_objects
    std::mutex   m;
    objmap      *map;
    
    batch              *current_batch;
    std::stack<batch*>  batches;
    std::map<int,char*> in_mem_objects;
    
    /* info on all live objects - all sizes in sectors
     */
    struct obj_info {
	uint32_t hdr;
	uint32_t data;
	uint32_t live;
	int      type;
    };
    std::map<int,obj_info> object_info;

    char      *super_name;
    char      *super;
    hdr       *super_h;
    super_hdr *super_sh;
    size_t     super_len;

    thread_pool<batch*> workers;
    thread_pool<int>    misc_threads;

    /* for flush()
     */
    int last_written;
    int last_pushed;
    std::condition_variable cv;
    
    char *read_object_hdr(const char *name, bool fast) {
	hdr *h = (hdr*)malloc(4096);
	if (io->read_object(name, (char*)h, 0, 4096) < 0)
	    goto fail;
	if (fast)
	    return (char*)h;
	if (h->hdr_sectors > 8) {
	    h = (hdr*)realloc(h, h->hdr_sectors * 512);
	    if (io->read_object(name, (char*)h, 0, h->hdr_sectors*512) < 0)
		goto fail;
	}
	return (char*)h;
    fail:
	free((char*)h);
	return NULL;
    }

    /* clone_info is variable-length, so we need to pass back pointers 
     * rather than values. That's OK because we allocate superblock permanently
     */
    typedef clone_info *clone_p;
    ssize_t read_super(const char *name, std::vector<uint32_t> &ckpts,
		       std::vector<clone_p> &clones, std::vector<snap_info> &snaps) {
	super = read_object_hdr(name, false);
	super_h = (hdr*)super;
	super_len = super_h->hdr_sectors * 512;

	if (super_h->magic != LSVD_MAGIC || super_h->version != 1 ||
	    super_h->type != LSVD_SUPER)
	    return -1;
	memcpy(my_uuid, super_h->vol_uuid, sizeof(uuid_t));

	super_sh = (super_hdr*)(super_h+1);

	decode_offset_len<uint32_t>(super, super_sh->ckpts_offset, super_sh->ckpts_len, ckpts);
	decode_offset_len<snap_info>(super, super_sh->snaps_offset, super_sh->snaps_len, snaps);

	// this one stores pointers, not values...
	clone_info *p_clone = (clone_info*)(super + super_sh->clones_offset),
	    *end_clone = (clone_info*)(super + super_sh->clones_offset + super_sh->clones_len);
	for (; p_clone < end_clone; p_clone++)
	    clones.push_back(p_clone);

	return super_sh->vol_size * 512;
    }

    ssize_t read_data_hdr(int seq, hdr &h, data_hdr &dh, std::vector<uint32_t> &ckpts,
		       std::vector<obj_cleaned> &cleaned, std::vector<data_map> &dmap) {
	auto name = io->object_name(seq);
	char *buf = read_object_hdr(name.c_str(), false);
	if (buf == NULL)
	    return -1;
	hdr      *tmp_h = (hdr*)buf;
	data_hdr *tmp_dh = (data_hdr*)(tmp_h+1);
	if (tmp_h->type != LSVD_DATA) {
	    free(buf);
	    return -1;
	}

	h = *tmp_h;
	dh = *tmp_dh;

	decode_offset_len<uint32_t>(buf, tmp_dh->ckpts_offset, tmp_dh->ckpts_len, ckpts);
	decode_offset_len<obj_cleaned>(buf, tmp_dh->objs_cleaned_offset, tmp_dh->objs_cleaned_len, cleaned);
	decode_offset_len<data_map>(buf, tmp_dh->map_offset, tmp_dh->map_len, dmap);

	free(buf);
	return 0;
    }

    ssize_t read_checkpoint(int seq, std::vector<uint32_t> &ckpts, std::vector<ckpt_obj> &objects, 
			    std::vector<deferred_delete> &deletes, std::vector<ckpt_mapentry> &dmap) {
	auto name = io->object_name(seq);
	char *buf = read_object_hdr(name.c_str(), false);
	if (buf == NULL)
	    return -1;
	hdr      *h = (hdr*)buf;
	ckpt_hdr *ch = (ckpt_hdr*)(h+1);
	if (h->type != LSVD_CKPT) {
	    free(buf);
	    return -1;
	}

	decode_offset_len<uint32_t>(buf, ch->ckpts_offset, ch->ckpts_len, ckpts);
	decode_offset_len<ckpt_obj>(buf, ch->objs_offset, ch->objs_len, objects);
	decode_offset_len<deferred_delete>(buf, ch->deletes_offset, ch->deletes_len, deletes);
	decode_offset_len<ckpt_mapentry>(buf, ch->map_offset, ch->map_len, dmap);

	free(buf);
	return 0;
    }

    int write_checkpoint(int seq) {
	std::vector<ckpt_mapentry> entries;
	std::vector<ckpt_obj> objects;
	
	std::unique_lock lk(map->m);
	last_ckpt = seq;
	for (auto it = map->map.begin(); it != map->map.end(); it++) {
	    auto [base, limit, ptr] = it->vals();
	    entries.push_back((ckpt_mapentry){.lba = base, .len = limit-base,
			.obj = (int32_t)ptr.obj, .offset = (int32_t)ptr.offset});
	}
	lk.unlock();
	size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);

	std::unique_lock lk2(m);
	for (auto it = object_info.begin(); it != object_info.end(); it++) {
	    auto obj_num = it->first;
	    auto [hdr, data, live, type] = it->second;
	    if (type == LSVD_DATA)
		objects.push_back((ckpt_obj){.seq = (uint32_t)obj_num, .hdr_sectors = hdr,
			    .data_sectors = data, .live_sectors = live});
	}

	size_t objs_bytes = objects.size() * sizeof(ckpt_obj);
	size_t hdr_bytes = sizeof(hdr) + sizeof(ckpt_hdr);
	uint32_t sectors = div_round_up(hdr_bytes + sizeof(seq) + map_bytes + objs_bytes, 512);
	object_info[seq] = (obj_info){.hdr = sectors, .data = 0, .live = 0, .type = LSVD_CKPT};
	lk2.unlock();

	char *buf = (char*)calloc(hdr_bytes, 1);
	hdr *h = (hdr*)buf;
	*h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_CKPT, .seq = (uint32_t)seq, .hdr_sectors = sectors,
		   .data_sectors = 0};
	memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));
	ckpt_hdr *ch = (ckpt_hdr*)(h+1);

	uint32_t o1 = sizeof(hdr)+sizeof(ckpt_hdr), o2 = o1 + sizeof(seq), o3 = o2 + objs_bytes;
	*ch = (ckpt_hdr){.ckpts_offset = o1, .ckpts_len = sizeof(seq),
			 .objs_offset = o2, .objs_len = o3-o2,
			 .deletes_offset = 0, .deletes_len = 0,
			 .map_offset = o3, .map_len = (uint32_t)map_bytes};

	iovec iov[] = {{.iov_base = buf, .iov_len = hdr_bytes},
		       {.iov_base = (char*)&seq, .iov_len = sizeof(seq)},
		       {.iov_base = (char*)objects.data(), objs_bytes},
		       {.iov_base = (char*)entries.data(), map_bytes}};
	io->write_numbered_object(seq, iov, 4);

	size_t offset = sizeof(*super_h) + sizeof(*super_sh);
	super_sh->ckpts_offset = offset;
	super_sh->ckpts_len = sizeof(seq);
	*(int*)(super + offset) = seq;
	struct iovec iov2 = {super, 4096};
	io->write_object(super_name, &iov2, 1);
	return seq;
    }
    
    int make_hdr(char *buf, batch *b) {
	hdr *h = (hdr*)buf;
	data_hdr *dh = (data_hdr*)(h+1);
	uint32_t o1 = sizeof(*h) + sizeof(*dh), l1 = sizeof(uint32_t),
	    o2 = o1 + l1, l2 = b->entries.size() * sizeof(data_map),
	    hdr_bytes = o2 + l2;
	lba_t hdr_sectors = div_round_up(hdr_bytes, 512);
	
	*h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_DATA, .seq = (uint32_t)b->seq,
		   .hdr_sectors = hdr_sectors,
		   .data_sectors = (uint32_t)(b->len / 512)};
	memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));

	*dh = (data_hdr){.last_data_obj = (uint32_t)b->seq, .ckpts_offset = o1, .ckpts_len = l1,
			 .objs_cleaned_offset = 0, .objs_cleaned_len = 0,
			 .map_offset = o2, .map_len = l2};

	uint32_t *p_ckpt = (uint32_t*)(dh+1);
	*p_ckpt = last_ckpt;

	data_map *dm = (data_map*)(p_ckpt+1);
	for (auto e : b->entries)
	    *dm++ = e;

	return (char*)dm - (char*)buf;
    }

    
    void worker_thread(thread_pool<batch*> *p) {
	while (p->running) {
	    std::unique_lock<std::mutex> lk(m);
	    batch *b;
	    if (!p->get_locked(lk, b))
		return;
	    uint32_t hdr_sectors = div_round_up(b->hdrlen(), 512);
	    object_info[b->seq] = (obj_info){.hdr = hdr_sectors, .data = (uint32_t)(b->len/512),
					     .live = (uint32_t)(b->len/512), .type = LSVD_DATA};
	    lk.unlock();

	    char *hdr = (char*)calloc(hdr_sectors*512, 1);
	    make_hdr(hdr, b);
	    iovec iov[2] = {{hdr, (size_t)(hdr_sectors*512)}, {b->buf, b->len}};
	    io->write_numbered_object(b->seq, iov, 2);
	    free(hdr);

	    lk.lock();
	    std::unique_lock objlock(map->m);
	    auto offset = hdr_sectors;
	    for (auto e : b->entries) {
		std::vector<extmap::lba2obj> deleted;
		extmap::obj_offset oo = {b->seq, offset};
		map->map.update(e.lba, e.lba+e.len, oo, &deleted);
		for (auto d : deleted) {
		    auto [base, limit, ptr] = d.vals();
		    if (ptr.obj != b->seq)
			object_info[ptr.obj].live -= (limit - base);
		}
		offset += e.len;
	    }

	    last_written = std::max(last_written, b->seq); // for flush()
	    cv.notify_all();
	    
	    in_mem_objects.erase(b->seq); // no need to free, since we re-use the batch
	    batches.push(b);
	    objlock.unlock();

	    lk.unlock();
	}
    }

    void ckpt_thread(thread_pool<int> *p) {
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

    void flush_thread(thread_pool<int> *p) {
	auto wait_time = std::chrono::milliseconds(500);
	auto timeout = std::chrono::seconds(2);
	auto t0 = std::chrono::system_clock::now();
	auto seq0 = batch_seq;

	while (p->running) {
	    std::unique_lock<std::mutex> lk(*p->m);
	    p->cv.wait_for(lk, wait_time);
	    if (p->running && current_batch && seq0 == batch_seq && current_batch->len > 0) {
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

public:
    backend *io;
    bool     nocache;
    
    translate(backend *_io, objmap *omap) : workers(&m), misc_threads(&m) {
	io = _io;
	map = omap;
	current_batch = NULL;
	nocache = false;
	last_written = last_pushed = 0;
    }
    ~translate() {
	while (!batches.empty()) {
	    auto b = batches.top();
	    batches.pop();
	    delete b;
	}
	if (current_batch)
	    delete current_batch;
	if (super)
	    free(super);
    }
    
    /* returns sequence number of ckpt
     */
    int checkpoint(void) {
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

    int flush(void) {
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

    ssize_t init(const char *name, int nthreads, bool timedflush) {
	std::vector<uint32_t>  ckpts;
	std::vector<clone_p>   clones;
	std::vector<snap_info> snaps;

	super_name = strdup(name);
	ssize_t bytes = read_super(name, ckpts, clones, snaps);
	if (bytes < 0)
	  return bytes;
	batch_seq = super_sh->next_obj;

	int _ckpt = 1;
	for (auto ck : ckpts) {
	    ckpts.resize(0);
	    std::vector<ckpt_obj> objects;
	    std::vector<deferred_delete> deletes;
	    std::vector<ckpt_mapentry> entries;
	    if (read_checkpoint(ck, ckpts, objects, deletes, entries) < 0)
		return -1;
	    for (auto o : objects) {
		object_info[o.seq] = (obj_info){.hdr = o.hdr_sectors, .data = o.data_sectors,
					.live = o.live_sectors, .type = LSVD_DATA};

	    }
	    for (auto m : entries) {
		map->map.update(m.lba, m.lba + m.len,
				(extmap::obj_offset){.obj = m.obj, .offset = m.offset});
	    }
	    _ckpt = ck;
	}

	for (int i = _ckpt; ; i++) {
	    std::vector<uint32_t>    ckpts;
	    std::vector<obj_cleaned> cleaned;
	    std::vector<data_map>    entries;
	    hdr h; data_hdr dh;
	    batch_seq = i;
	    if (read_data_hdr(i, h, dh, ckpts, cleaned, entries) < 0)
		break;
	    object_info[i] = (obj_info){.hdr = h.hdr_sectors, .data = h.data_sectors,
					.live = h.data_sectors, .type = LSVD_DATA};
	    int offset = 0, hdr_len = h.hdr_sectors;
	    for (auto m : entries) {
		map->map.update(m.lba, m.lba + m.len,
				(extmap::obj_offset){.obj = i, .offset = offset + hdr_len});
		offset += m.len;
	    }
	}

	for (int i = 0; i < nthreads; i++) 
	    workers.pool.push(std::thread(&translate::worker_thread, this, &workers));
	misc_threads.pool.push(std::thread(&translate::ckpt_thread, this, &misc_threads));
	if (timedflush)
	    misc_threads.pool.push(std::thread(&translate::flush_thread, this, &misc_threads));

	return bytes;
    }

    void shutdown(void) {
    }

    ssize_t writev(size_t offset, iovec *iov, int iovcnt) {
	std::unique_lock<std::mutex> lk(m);
	size_t len = iov_sum(iov, iovcnt);

	if (current_batch && current_batch->len + len > current_batch->max) {
	    last_pushed = current_batch->seq;
	    workers.put_locked(current_batch);
	    current_batch = NULL;
	}
	if (current_batch == NULL) {
	    if (batches.empty())
		current_batch = new batch(BATCH_SIZE);
	    else {
		current_batch = batches.top();
		batches.pop();
	    }
	    current_batch->reset();
	    if (nocache)
		in_mem_objects[current_batch->seq] = current_batch->buf;
	}

	int64_t sector_offset = current_batch->len / 512,
	    lba = offset/512, limit = (offset+len)/512;
	current_batch->append_iov(offset / 512, iov, iovcnt);

	if (nocache) {
	    std::vector<extmap::lba2obj> deleted;
	    extmap::obj_offset oo = {current_batch->seq, sector_offset};
	    std::unique_lock objlock(map->m);

	    map->map.update(lba, limit, oo, &deleted);
	    for (auto d : deleted) {
		auto [base, limit, ptr] = d.vals();
		object_info[ptr.obj].live -= (limit - base);
	    }
	}
	
	return len;
    }

    ssize_t write(size_t offset, size_t len, char *buf) {
	iovec iov = {buf, len};
	return this->writev(offset, &iov, 1);
    }

    ssize_t read(size_t offset, size_t len, char *buf) {
	int64_t base = offset / 512;
	int64_t sectors = len / 512, limit = base + sectors;

	if (map->map.size() == 0) {
	    memset(buf, 0, len);
	    return len;
	}

	/* object number, offset (bytes), length (bytes) */
	std::vector<std::tuple<int, size_t, size_t>> regions;

	std::unique_lock lk(m);
	std::shared_lock slk(map->m);
	
	auto prev = base;
	char *ptr = buf;
	for (auto it = map->map.lookup(base); it != map->map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, oo] = it->vals(base, limit);
	    if (_base > prev) {	// unmapped
		size_t _len = (_base - prev)*512;
		regions.push_back(std::tuple(-1, 0, _len));
		ptr += _len;
	    }
	    size_t _len = (_limit - _base) * 512,
		_offset = oo.offset * 512;
	    int obj = oo.obj;
	    if (in_mem_objects.find(obj) != in_mem_objects.end()) {
		memcpy((void*)ptr, in_mem_objects[obj]+_offset, _len);
		obj = -2;
	    }
	    regions.push_back(std::tuple(obj, _offset, _len));
	    ptr += _len;
	    prev = _limit;
	}
	slk.unlock();
	lk.unlock();

	ptr = buf;
	for (auto [obj, _offset, _len] : regions) {
	    if (obj == -1)
		memset(ptr, 0, _len);
	    else if (obj == -2)
		/* skip */;
	    else
		io->read_numbered_object(obj, ptr, _offset, _len);
	    ptr += _len;
	}

	return ptr - buf;
    }

    // debug methods
    int inmem(int max, int *list) {
	int i = 0;
	for (auto it = in_mem_objects.begin(); i < max && it != in_mem_objects.end(); it++)
	    list[i++] = it->first;
	return i;
    }
    void getmap(int base, int limit, int (*cb)(void *ptr,int,int,int,int), void *ptr) {
	for (auto it = map->map.lookup(base); it != map->map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, oo] = it->vals(base, limit);
	    if (!cb(ptr, (int)_base, (int)_limit, (int)oo.obj, (int)oo.offset))
		break;
	}
    }
    int mapsize(void) {
	return map->map.size();
    }
    void reset(void) {
	map->map.reset();
    }
    int frontier(void) {
	if (current_batch)
	    return current_batch->len / 512;
	return 0;
    }
};

void throw_fs_error(std::string msg) {
    throw fs::filesystem_error(msg, std::error_code(errno, std::system_category()));
}

/* each read or write queues up one of these for a worker thread
 */
struct cache_work {
public:
    uint64_t  lba;
    void    (*callback)(void*);
    void     *ptr;
    lba_t     sectors;
    std::vector<iovec> iovs;
    cache_work(lba_t _lba, const iovec *iov, int iovcnt, void (*_callback)(void*), void *_ptr) {
	lba = _lba;
	int bytes = 0;
	for (int i = 0; i < iovcnt; i++) {
	    iovs.push_back(iov[i]);
	    bytes += iov[i].iov_len;
	}
	sectors = bytes / 512;
	callback = _callback;
	ptr = _ptr;
    }
};


#include "smartiov.cc"

/* the read cache is:
 * 1. indexed by obj/offset[*], not LBA
 * 2. stores aligned 64KB blocks 
 * 3. tolerates 4KB-aligned "holes" using a per-entry bitmap (64KB = 16 bits)
 * [*] offset is in units of 64KB blocks
 */
class read_cache {
    std::mutex m;
    std::map<extmap::obj_offset,int> map;

    j_read_super       *super;
    extmap::obj_offset *flat_map;
    uint16_t           *bitmap;
    objmap             *omap;
    translate          *be;
    int                 fd;
    backend            *io;
    
    int               unit_sectors;
    std::vector<int>  free_blks;
    bool              map_dirty;

    double            hit_rate;
    int               n_hits;
    int               n_misses;
    
    thread_pool<cache_work*> workers;
    thread_pool<int> misc_threads; // eviction thread, for now
    bool             nothreads;	   // for debug
    
    // cache block is busy when being written - mask can change, but not
    // its mapping.
    std::vector<bool> busy;
    std::condition_variable cv;

    /* this is kind of crazy. return a bitmap corresponding to the 4KB pages
     * in [base%unit, limit%unit), where base, limit, and unit are in sectors
     */
    typedef uint16_t mask_t;
    mask_t page_mask(int base, int limit, int unit) {
	int top = round_up(base+1, unit);
	limit = (limit < top) ? limit : top;
	int base_page = base/8, limit_page = div_round_up(limit, 8),
	    unit_page = unit / 8;
	mask_t val = 0;
	for (int i = base_page % unit_page; base_page < limit_page; base_page++, i++)
	    val |= (1 << i);
	return val;
    }

    /* evict 'n' blocks
     */
    void evict(int n) {
	assert(!m.try_lock());	// m must be locked
	std::uniform_int_distribution<int> uni(0,super->units - 1);
	for (int i = 0; i < n; i++) {
	    int j = uni(rng);
	    while (flat_map[j].obj == 0)
		j = uni(rng);
	    bitmap[j] = 0;
	    auto oo = flat_map[j];
	    flat_map[j] = (extmap::obj_offset){0, 0};
	    map.erase(oo);
	    free_blks.push_back(j);
	}
    }

    void evict_thread(thread_pool<int> *p) {
	auto wait_time = std::chrono::milliseconds(500);
	auto t0 = std::chrono::system_clock::now();
	auto timeout = std::chrono::seconds(2);

	std::unique_lock<std::mutex> lk(m);

	while (p->running) {
	    p->cv.wait_for(lk, wait_time);
	    if (!p->running)
		return;

	    if (!map_dirty)	// free list didn't change
		continue;

	    int n = 0;
	    if ((int)free_blks.size() < super->units / 16)
		n = super->units / 4 - free_blks.size();
	    if (n)
		evict(n);

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

    int n_lines_read = 0;
    
    void do_readv(size_t offset, const iovec *iov, int iovcnt) {
	auto iovs = smartiov(iov, iovcnt);
	size_t len = iovs.bytes();
	lba_t lba = offset/512, sectors = len/512;
	std::vector<std::tuple<lba_t,lba_t,extmap::obj_offset>> extents;
	std::shared_lock lk(omap->m);
	std::unique_lock lk2(m);
	for (auto it = omap->map.lookup(lba);
	     it != omap->map.end() && it->base() < lba+sectors; it++) 
	    extents.push_back(it->vals(lba, lba+sectors));

	if (n_hits + n_misses >= 10000) {
	    hit_rate = (hit_rate * 0.5) + (0.5 * n_hits) / (n_hits + n_misses);
	    //xprintf("hit rate: %f hits %d misses %d lines_read %d\n", hit_rate, n_hits, n_misses, n_lines_read);
	    n_hits = n_misses = 0;
	}
	/* hack for better random performance - if hit rate is less than 50%, gradually
	 * stop using the cache.
	 */
	std::uniform_real_distribution<double> unif(0.0,1.0);
	bool use_cache = unif(rng) < 2 * hit_rate;
	lk2.unlock();
	lk.unlock();

	std::vector<std::tuple<extmap::obj_offset,sector_t,char*>> to_add;
	size_t buf_offset = 0;
	
	for (auto e : extents) {
	    assert(len > 0);
	    
	    auto [base, limit, ptr] = e;
	    if (base > lba) {
		auto bytes = (base-lba)*512;
		iovs.slice(buf_offset, buf_offset+bytes).zero();
		len -= bytes;
	    }
	    while (base < limit) {
		extmap::obj_offset unit = {ptr.obj, ptr.offset / unit_sectors};

                // TODO: unit_sectors -> unit_nsectors
		sector_t blk_base_lba = unit.offset * unit_sectors;
                sector_t blk_offset = ptr.offset % unit_sectors;
                sector_t blk_top_offset = std::min({(int)(blk_offset+sectors),
			    round_up(blk_offset+1,unit_sectors),
			    (int)(blk_offset + (limit-base))});
		
		bool in_cache = false;
		int n = -1;

		lk2.lock();
		auto it = map.find(unit);
		if (it != map.end()) {
		    n = it->second;
		    mask_t access_mask = page_mask(blk_offset, blk_top_offset, unit_sectors);
		    if ((access_mask & bitmap[n]) == access_mask)
			in_cache = true;
		}
		if (in_cache)
		    n_hits++;
		else
		    n_misses++;
		lk2.unlock();

		if (in_cache) {
		    sector_t blk_in_ssd = super->base*8 + n*unit_sectors,
			start = blk_in_ssd + blk_offset,
			finish = start + (blk_top_offset - blk_offset);
		    size_t bytes = 512 * (finish - start);
		    
		    auto tmp = iovs.slice(buf_offset, buf_offset+bytes);
		    if (preadv(fd, tmp.data(), tmp.size(), 512*start) < 0)
			throw_fs_error("rcache");
		    
		    base += (finish - start);
		    ptr.offset += (finish - start); // HACK
		    buf_offset += bytes;
		    len -= bytes;
		}
		else if (!use_cache) {
		    lba_t sectors = limit - base;
		    char *buf = (char*)malloc(512 * sectors);
		    auto bytes = io->read_numbered_object(ptr.obj, buf, 512 * ptr.offset, 512 * sectors);
		    iovs.slice(buf_offset, buf_offset + 512 * sectors).copy_in(buf);
		    free(buf);
		    base = limit;
		    buf_offset += 512 * sectors;
		    len -= 512 * sectors;
		}
		else {
		    n_lines_read++;
		    char *cache_line = (char*)aligned_alloc(512, unit_sectors*512);
		    auto bytes = io->read_numbered_object(unit.obj, cache_line,
							  512*blk_base_lba, 512*unit_sectors);
                    size_t start = 512 * blk_offset,
			finish = 512 * blk_top_offset;
		    assert((int)finish <= bytes);

		    iovs.slice(buf_offset,
			       buf_offset+(finish-start)).copy_in(cache_line+start);

		    base += (blk_top_offset - blk_offset);
		    ptr.offset += (blk_top_offset - blk_offset); // HACK
		    buf_offset += (finish-start);
		    len -= (finish-start);
		    extmap::obj_offset ox = {unit.obj, unit.offset * unit_sectors};
		    to_add.push_back(std::tuple(ox, bytes/512, cache_line));
		}
	    }
	    lba = limit;
	}
	if (len > 0)
	    iovs.slice(buf_offset, buf_offset+len).zero();

	// now read is finished, and we can add shit to the cache
	for (auto [oo, n, cache_line] : to_add) {
	    add(oo, n, cache_line);
	    free(cache_line);
	}
    }

    void reader(thread_pool<cache_work*> *p) {
	while (p->running) {
	    cache_work *w;
	    if (!p->get(w))
		break;
	    do_readv(w->lba * 512, w->iovs.data(), w->iovs.size());
	    DBG((long)w->ptr);
	    w->callback(w->ptr);
	    delete w;
	}
    }

public:

    // TODO: iovec version
    void add(extmap::obj_offset oo, int sectors, char *buf) {
	// must be 4KB aligned
	assert(!(oo.offset & 7));

	while (sectors > 0) {
	    std::unique_lock lk(m);

	    extmap::obj_offset obj_blk = {oo.obj, oo.offset / unit_sectors};
	    int cache_blk = -1;
	    auto it = map.find(obj_blk);
	    if (it != map.end())
		cache_blk = it->second;
	    else if (free_blks.size() > 0) {
		cache_blk = free_blks.back();
		free_blks.pop_back();
	    }
	    else {
		//xprintf("ADD %d.%d: no space\n", oo.obj, oo.offset);
		return;
	    }
	    while (busy[cache_blk])
		cv.wait(lk);
	    busy[cache_blk] = true;
	    auto mask = bitmap[cache_blk];
	    lk.unlock();

	    assert(cache_blk >= 0);

	    auto obj_page = oo.offset / 8;
	    auto pages_in_blk = unit_sectors / 8;
	    auto blk_page = obj_blk.offset * pages_in_blk;
	    std::vector<iovec> iov;

	    for (int i = obj_page - blk_page; sectors > 0 && i < pages_in_blk; i++) {
		mask |= (1 << i);
		iov.push_back((iovec){buf, 4096});
		buf += 4096;
		sectors -= 8;
		oo.offset += 8;
	    }

	    off_t blk_offset = ((cache_blk * pages_in_blk) + super->base) * 4096;
	    blk_offset += (obj_page - blk_page) * 4096;
	    if (pwritev(fd, iov.data(), iov.size(), blk_offset) < 0)
		throw_fs_error("rcache");

	    lk.lock();
	    map[obj_blk] = cache_blk;
	    bitmap[cache_blk] = mask;
	    flat_map[cache_blk] = obj_blk;
	    busy[cache_blk] = false;
	    map_dirty = true;
	    cv.notify_one();
	    lk.unlock();
	}
    }

    void readv(size_t offset, const iovec *iov, int iovcnt, void (*callback)(void*), void *ptr) {
	workers.put(new cache_work(offset/512, iov, iovcnt, callback, ptr));
    }

#if 0
    void read(size_t offset, size_t len, char *buf) {
	iovec iov = {buf, len};
	return readv(offset, &iov, 1);
    }
#endif

    read_cache(uint32_t blkno, int _fd, bool nt, translate *_be, objmap *_om, backend *_io) :
	omap(_om), be(_be), fd(_fd), io(_io), hit_rate(1.0), n_hits(0), n_misses(0), workers(&m),
	misc_threads(&m), nothreads(nt)
    {
	n_lines_read = 0;
	char *buf = (char*)aligned_alloc(512, 4096);
	if (pread(fd, buf, 4096, 4096*blkno) < 4096)
	    throw_fs_error("rcache");
	super = (j_read_super*)buf;
	
	assert(super->unit_size == 128); // 64KB, in sectors
	unit_sectors = super->unit_size; // todo: fixme
	
	int oos_per_pg = 4096 / sizeof(extmap::obj_offset);
	assert(div_round_up(super->units, oos_per_pg) == super->map_blocks);
	assert(div_round_up(super->units, 2048) == super->bitmap_blocks);

	flat_map = (extmap::obj_offset*)aligned_alloc(512, super->map_blocks*4096);
	if (pread(fd, (char*)flat_map, super->map_blocks*4096, super->map_start*4096) < 0)
	    throw_fs_error("rcache2");
	bitmap = (uint16_t*)aligned_alloc(512, super->bitmap_blocks*4096);
	if (pread(fd, (char*)bitmap, super->bitmap_blocks*4096, super->bitmap_start*4096) < 0)
	    throw_fs_error("rcache3");

	for (int i = 0; i < super->units; i++)
	    if (flat_map[i].obj != 0) {
		map[flat_map[i]] = i;
	    }
	    else {
		free_blks.push_back(i);
		bitmap[i] = 0;
	    }

	busy.reserve(super->units);
	for (int i = 0; i < super->units; i++)
	    busy.push_back(false);
	map_dirty = false;

	for (int i = 0; i < 6; i++)
	    workers.pool.push(std::thread(&read_cache::reader, this, &workers));
	if (!nothreads)
	    misc_threads.pool.push(std::thread(&read_cache::evict_thread, this, &misc_threads));
    }

    void write_map(void) {
	pwrite(fd, flat_map, 4096 * super->map_blocks, 4096 * super->map_start);
	pwrite(fd, bitmap, 4096 * super->bitmap_blocks, 4096 * super->bitmap_start);
    }
    
    ~read_cache() {
	free((void*)flat_map);
	free((void*)bitmap);
	free((void*)super);
    }

    /* debugging
     */
    void get_info(j_read_super **p_super, extmap::obj_offset **p_flat, uint16_t **p_bitmap,
		  std::vector<int> **p_free_blks, std::map<extmap::obj_offset,int> **p_map) {
	if (p_super != NULL)
	    *p_super = super;
	if (p_flat != NULL)
	    *p_flat = flat_map;
	if (p_bitmap != NULL)
	    *p_bitmap = bitmap;
	if (p_free_blks != NULL)
	    *p_free_blks = &free_blks;
	if (p_map != NULL)
	    *p_map = &map;
    }

    void do_evict(int n) {
	std::unique_lock lk(m);
	evict(n);
    }
    void reset(void) {
    }
    
};

/* simple backend that uses files in a directory. 
 * good for debugging and testing
 */
class file_backend : public backend {
    char *prefix;
public:
    file_backend(const char *_prefix) {
	prefix = strdup(_prefix);
    }
    ~file_backend() {
	free((void*)prefix);
    }
    ssize_t write_object(const char *name, iovec *iov, int iovcnt) {
	int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0777);
	if (fd < 0)
	    return -1;
	auto val = writev(fd, iov, iovcnt);
	close(fd);
	return val;
    }
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) {
	auto name = std::string(prefix) + "." + hex(seq);
	return write_object(name.c_str(), iov, iovcnt);
    }
    ssize_t read_object(const char *name, char *buf, size_t offset, size_t len) {
	int fd = open(name, O_RDONLY);
	if (fd < 0)
	    return -1;
	auto val = pread(fd, buf, len, offset);
	close(fd);
	if (val < 0)
	    throw_fs_error("read_obj");
	return val;
    }
    ssize_t read_numbered_object(int seq, char *buf, size_t offset, size_t len) {
	auto name = std::string(prefix) + "." + hex(seq);
	return read_object(name.c_str(), buf, offset, len);
    }
    std::string object_name(int seq) {
	return std::string(prefix) + "." + hex(seq);
    }
};    


static bool aligned(void *ptr, int a)
{
    return 0 == ((long)ptr & (a-1));
}


typedef int page_t;

/* all addresses are in units of 4KB blocks
 */
class write_cache {
    int            fd;
    uint32_t       super_blkno;
    j_write_super *super;	// 4KB

    extmap::cachemap2 map;
    extmap::cachemap2 rmap;
    std::map<page_t,int> lengths;
    translate        *be;
    bool              map_dirty;
    
    thread_pool<cache_work*> workers;
    thread_pool<int>          *misc_threads;
    std::mutex                m;
    std::condition_variable   alloc_cv;
    int                       nfree;
    
    char *pad_page;
    
    int pages_free(uint32_t oldest) {
	auto size = super->limit - super->base;
	auto tail = (super->next >= oldest) ? oldest + size : oldest;
	return tail - super->next - 1;
    }
    
    uint32_t allocate(page_t n, page_t &pad) {
	std::unique_lock lk(m);
	auto timeout = std::chrono::seconds(1);
	while (pages_free(super->oldest) < 2*n) {
	    alloc_cv.wait_for(lk, timeout);
	    if (pages_free(super->oldest) < 2*n)
		printf("\nwaiting(1): %d %d %d\n", pages_free(super->oldest), super->next, super->oldest);
	}
	
	pad = 0;
	if (super->limit - super->next < (uint32_t)n) {
	    pad = super->next;
	    super->next = super->base;
	}
	auto val = super->next;
	super->next += n;
	if (super->next == super->limit)
	    super->next = super->base;
	return val;
    }

    j_hdr *mk_header(char *buf, uint32_t type, uuid_t &uuid, page_t blks) {
	memset(buf, 0, 4096);
	j_hdr *h = (j_hdr*)buf;
	*h = (j_hdr){.magic = LSVD_MAGIC, .type = type, .version = 1, .vol_uuid = {0},
		     .seq = super->seq++, .len = (uint32_t)blks, .crc32 = 0,
		     .extent_offset = 0, .extent_len = 0};
	memcpy(h->vol_uuid, uuid, sizeof(uuid));
	return h;
    }

    void writer(thread_pool<cache_work*> *p) {
	char *buf = (char*)aligned_alloc(512, 4096); // for direct I/O
	auto period = std::chrono::milliseconds(25);
	const int write_max_pct = 2;
	const int write_max_kb = 512;
	int max_sectors = std::min((write_max_pct * (int)(super->limit - super->base) * 8 / 100),
				   write_max_kb * 2);
	
	while (p->running) {
	    std::vector<char*> bounce_bufs;
	    std::unique_lock lk(m);
	    if (!p->wait_locked(lk))
		break;

	    std::vector<cache_work*> work;
	    int sectors = 0;
	    while (p->running && sectors < max_sectors) {
		cache_work *w;
		if (!p->get_nowait(w))
		    break;
		sectors += w->sectors;
		work.push_back(w);
	    }
	    lk.unlock();
	    if (!p->running)
		break;
	    
	    page_t blocks = div_round_up(sectors, 8);
	    // allocate blocks + 1
	    page_t pad, blockno = allocate(blocks+1, pad);

	    if (pad != 0) {
		mk_header(buf, LSVD_J_PAD, my_uuid, (super->limit - pad));
		assert(pad < (int)super->limit);
		if (pwrite(fd, buf, 4096, pad*4096) < 0)
		    throw_fs_error("wpad");
	    }
	    
	    std::vector<j_extent> extents;
	    for (auto w : work) {
		for (int i = 0; i < (int)w->iovs.size(); i++) {
		    if (aligned(w->iovs[i].iov_base, 512))
			continue;
		    char *p = (char*)aligned_alloc(512, w->iovs[i].iov_len);
		    memcpy(p, w->iovs[i].iov_base, w->iovs[i].iov_len);
		    w->iovs[i].iov_base = p;
		    bounce_bufs.push_back(p);
		}
		extents.push_back((j_extent){w->lba, iov_sum(w->iovs.data(), w->iovs.size())/512});
	    }

	    j_hdr *j = mk_header(buf, LSVD_J_DATA, my_uuid, 1+blocks);
	    j->extent_offset = sizeof(*j);
	    size_t e_bytes = extents.size() * sizeof(j_extent);
	    j->extent_len = e_bytes;
	    memcpy((void*)(buf + sizeof(*j)), (void*)extents.data(), e_bytes);
		
	    auto iovs = smartiov();
	    iovs.push_back((iovec){buf, 4096});

	    for (auto w : work)
		for (auto i : w->iovs)
		    iovs.push_back(i);
		    
	    sector_t pad_sectors = blocks*8 - sectors;
	    if (pad_sectors > 0)
		iovs.push_back((iovec){pad_page, (size_t)pad_sectors*512});

	    assert(blockno + div_round_up(iovs.bytes(), 4096) <= (int)super->limit);
	    if (pwritev(fd, iovs.data(), iovs.size(), blockno*4096) < 0)
		throw_fs_error("wdata");

	    /* update map first under lock. 
	     * Note that map is in units of *sectors*, not blocks 
	     */
	    lk.lock();
	    if (pad != 0)
		lengths[pad] = super->limit - pad;
	    lengths[blockno] = blocks+1;
	    lba_t plba = (blockno+1) * 8;
	    std::vector<extmap::lba2lba> garbage;
	    for (auto w : work) {
		map.update(w->lba, w->lba + w->sectors, plba, &garbage);
		rmap.update(plba, plba+w->sectors, w->lba);
		plba += sectors;
		map_dirty = true;
	    }
	    for (auto it = garbage.begin(); it != garbage.end(); it++) {
		rmap.trim(it->s.base, it->s.base+it->s.len);
	    }
	    lk.unlock();
	    
	    /* then send to backend */
	    for (auto w : work) {
		be->writev(w->lba*512, w->iovs.data(), w->iovs.size());
		w->callback(w->ptr);
	    }

	    while (!bounce_bufs.empty()) {
		free(bounce_bufs.back());
		bounce_bufs.pop_back();
	    }
	    for (auto w : work)
		delete w;
	}
	free(buf);
    }
    
    /* cache eviction - get info from oldest entry in cache. [should be private]
     *  @blk - header to read
     *  @extents - data to move. empty if J_PAD or J_CKPT
     *  return value - first page of next record
     */
    page_t get_oldest(page_t blk, std::vector<j_extent> &extents) {
	char *buf = (char*)aligned_alloc(512, 4096);
	j_hdr *h = (j_hdr*)buf;

	if (pread(fd, buf, 4096, blk*4096) < 0)
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

	return next_blk;
    }

    /* min free is min(5%, 100MB). Free space:
     *  N = limit - base
     *  oldest == newest : free = N-1
     *  else : free = ((oldest + N) - newest - 1) % N
     */
    void get_exts_to_evict(std::vector<j_extent> &exts_in, page_t pg_base, page_t pg_limit, 
			   std::vector<j_extent> &exts_out) {
	std::unique_lock<std::mutex> lk(m);
	for (auto e : exts_in) {
	    lba_t base = e.lba, limit = e.lba + e.len;
	    for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
		auto [_base, _limit, ptr] = it->vals(base, limit);
		if (pg_base*8 <= ptr && ptr < pg_limit*8)
		    exts_out.push_back((j_extent){.lba = (uint64_t)_base,
				.len = (uint64_t)(_limit-_base)});
	    }
	}
    }
    
    void evict_thread(thread_pool<int> *p) {
	auto period = std::chrono::milliseconds(25);
	const int evict_min_pct = 5;
	const int evict_max_mb = 100;
	int trigger = std::min((evict_min_pct * (int)(super->limit - super->base) / 100),
			       evict_max_mb * (1024*1024/4096));
	auto t0 = std::chrono::system_clock::now();
	auto super_timeout = std::chrono::milliseconds(500);
	
//	int trigger = 100;

	while (p->running) {
	    std::unique_lock<std::mutex> lk(m);
	    p->cv.wait_for(lk, period);
	    if (!p->running)
		return;
	    int pgs_free = pages_free(super->oldest);
	    if (super->oldest != super->next && pgs_free <= trigger) {
		auto oldest = super->oldest;
		std::vector<j_extent> to_delete;

		while (pages_free(oldest) < trigger*3) {
		    /*
		     * for each record we trim from the cache, collect the vLBA->pLBA
		     * mappings that we need to remove. Note that we keep the revers map
		     * up to date, so we don't need to check them against the forward map.
		     */
		    auto len = lengths[oldest];
		    assert(len > 0);
		    lengths.erase(oldest);
		    lba_t base = (oldest+1)*8, limit = base + (len-1)*8;
		    for (auto it = rmap.lookup(base); it != rmap.end() && it->base() < limit; it++) {
			auto [p_base, p_limit, vlba] = it->vals(base, limit);
			to_delete.push_back((j_extent){(uint64_t)vlba, (uint64_t)p_limit-p_base});
		    }
		    rmap.trim(base, limit);
		    oldest += len;
		    if (oldest >= super->limit)
			oldest = super->base;
		}
		
		/* TODO: unlock, read the data and add to read cache, re-lock
		 */

		super->oldest = oldest;		
		for (auto e : to_delete)
		    map.trim(e.lba, e.lba + e.len);
		alloc_cv.notify_all();

		lk.unlock();
		auto t = std::chrono::system_clock::now();
		if (t - t0 >= super_timeout) {
		    if (pwrite(fd, super, 4096, 4096*super_blkno) < 0)
			throw_fs_error("wsuper_rewrite");
		    t0 = t;
		}
	    }
	}
    }

    void ckpt_thread(thread_pool<int> *p) {
	auto next0 = super->next, N = super->limit - super->base;
	auto period = std::chrono::milliseconds(100);
	const int ckpt_interval = N / 8;

	while (p->running) {
	    std::unique_lock lk(m);
	    p->cv.wait_for(lk, period);
	    if (p->running && (int)(super->next + N - next0) > ckpt_interval) {
		next0 = super->next;
		lk.unlock();
		write_checkpoint();
	    }
	}
    }

    void write_checkpoint(void) {
	char *buf = (char*)aligned_alloc(512, 4096);
	j_write_super *super_copy = (j_write_super*)aligned_alloc(512, 4096);

	std::unique_lock<std::mutex> lk(m);
	size_t map_bytes = map.size() * sizeof(j_map_extent);
	size_t len_bytes = lengths.size() * sizeof(j_length);
	page_t map_pages = div_round_up(map_bytes, 4096), len_pages = div_round_up(len_bytes, 4096),
	    ckpt_pages = map_pages + len_pages;
	lk.unlock();		// TODO: HACK
	page_t pad, blockno = allocate(ckpt_pages+1, pad);
	lk.lock();

	std::vector<j_map_extent> extents;
	for (auto it = map.begin(); it != map.end(); it++)
	    extents.push_back((j_map_extent){(uint64_t)it->s.base, (uint64_t)it->s.len,
			(uint32_t)it->s.ptr/8});
	memcpy(super_copy, super, 4096);
	std::vector<j_length> _lengths;
	for (auto it = lengths.begin(); it != lengths.end(); it++)
	    _lengths.push_back((j_length){it->first, it->second});
	lk.unlock();

	if (pad != 0) {
	    mk_header(buf, LSVD_J_PAD, my_uuid, (super->limit - pad));
	    if (pwrite(fd, buf, 4096, pad*4096) < 0)
		throw_fs_error("wckpt_pad");
	}

	mk_header(buf, LSVD_J_CKPT, my_uuid, 1+ckpt_pages);
	char *e_buf = (char*)aligned_alloc(512, 4096*ckpt_pages); // bounce buffer
	memcpy(e_buf, extents.data(), map_bytes);
	if (map_bytes % 4096)	// make valgrind happy
	    memset(e_buf + map_bytes, 0, 4096 - (map_bytes % 4096));

	char *l_buf = e_buf + map_pages*4096;
	memcpy(l_buf, _lengths.data(), len_bytes);
	if (len_bytes % 4096)	// make valgrind happy
	    memset(l_buf + len_bytes, 0, 4096 - (len_bytes % 4096));
	
	super_copy->map_start = super->map_start = blockno+1;
	super_copy->map_blocks = super->map_blocks = map_pages;
	super_copy->map_entries = super->map_entries = extents.size();

	super_copy->len_start = super->len_start = blockno+1+map_pages;
	super_copy->len_blocks = super->len_blocks = len_pages;
	super_copy->len_entries = super->len_entries = lengths.size();
	
	std::vector<iovec> iovs;
	iovs.push_back((iovec){buf, 4096});
	iovs.push_back((iovec){e_buf, (size_t)4096*ckpt_pages});

	if (pwritev(fd, iovs.data(), iovs.size(), 4096*blockno) < 0)
	    throw_fs_error("wckpt_e");
	if (pwrite(fd, (char*)super_copy, 4096, 4096*super_blkno) < 0)
	    throw_fs_error("wckpt_s");

	free(buf);
	free(super_copy);
	free(e_buf);
	map_dirty = false;
    }

public:
    typedef std::tuple<size_t,size_t,size_t> cache_miss;
    
    class aio_readv_work {
    public:
	size_t offset;
	smartiov iovs;
	std::vector<cache_miss> *misses;
	void (*cb)(void*);
	void *ptr;
	aio_readv_work() : offset(0), misses(NULL), cb(NULL), ptr(NULL){}
	aio_readv_work(size_t _offset, const iovec *iov, int iovcnt, std::vector<cache_miss> *_misses,
		       void (*_cb)(void*), void *_ptr) : offset(_offset), iovs(iov, iovcnt), misses(_misses), cb(_cb), ptr(_ptr) {}
    };

    thread_pool<aio_readv_work*> readv_workers;
    
    void aio_readv_thread(thread_pool<aio_readv_work*> *p) {
	while (p->running) {
	    aio_readv_work *w;
	    if (!p->get(w)) {
		break;
	    }
	    readv(w->offset, w->iovs.data(), w->iovs.size(), w->misses);
	    w->cb(w->ptr);
	}
    }

    write_cache(uint32_t blkno, int _fd, translate *_be, int n_threads) : workers(&m), readv_workers(&m) {
	super_blkno = blkno;
	fd = _fd;
	be = _be;
	char *buf = (char*)aligned_alloc(512, 4096);
	if (pread(fd, buf, 4096, 4096*blkno) < 4096)
	    throw_fs_error("wcache");
	super = (j_write_super*)buf;
	pad_page = (char*)aligned_alloc(512, 4096);
	memset(pad_page, 0, 4096);
	map_dirty = false;
	
	if (super->map_entries) {
	    size_t map_bytes = super->map_entries * sizeof(j_map_extent),
		map_bytes_rounded = round_up(map_bytes, 4096);
	    char *map_buf = (char*)aligned_alloc(512, map_bytes_rounded);
	    std::vector<j_map_extent> extents;
	    if (pread(fd, map_buf, map_bytes_rounded, 4096 * super->map_start) < 0)
		throw_fs_error("wcache_map");
	    decode_offset_len<j_map_extent>(map_buf, 0, map_bytes, extents);
	    for (auto e : extents) {
		map.update(e.lba, e.lba+e.len, e.page*8);
		rmap.update(e.page * 8, e.page*8 + e.len, e.lba);
	    }
	    free(map_buf);

	    size_t len_bytes = super->len_entries * sizeof(j_length),
		len_bytes_rounded = round_up(len_bytes, 4096);
	    char *len_buf = (char*)aligned_alloc(512, len_bytes_rounded);
	    std::vector<j_length> _lengths;
	    if (pread(fd, len_buf, len_bytes_rounded, 4096 * super->len_start) < 0)
		throw_fs_error("wcache_len");
	    decode_offset_len<j_length>(len_buf, 0, len_bytes, _lengths);
	    for (auto l : _lengths) 
		lengths[l.page] = l.len;
	    free(len_buf);
	}

	/* TODO TODO TODO - need to roll log forward
	 */
	auto N = super->limit - super->base;
	if (super->oldest == super->next)
	    nfree = N - 1;
	else
	    nfree = (super->oldest + N - super->next) % N;

	// https://stackoverflow.com/questions/22657770/using-c-11-multithreading-on-non-static-member-function
	for (auto i = 0; i < n_threads; i++)
	    workers.pool.push(std::thread(&write_cache::writer, this, &workers));
	for (auto i = 0; i < 6; i++)
	    readv_workers.pool.push(std::thread(&write_cache::aio_readv_thread, this, &readv_workers));

	misc_threads = new thread_pool<int>(&m);
	misc_threads->pool.push(std::thread(&write_cache::evict_thread, this, misc_threads));
    }
    ~write_cache() {
	free(pad_page);
	free(super);
	delete misc_threads;
    }

    void write(size_t offset, const iovec *iov, int iovcnt,
	       void (*callback)(void*), void *ptr) {
	workers.put(new cache_work(offset/512, iov, iovcnt, callback, ptr));
    }

   /* returns tuples of:
     * offset, len, buf_offset
     */

    // TODO: how the hell to handle fragments?
    void readv(size_t offset, const iovec *iov, int iovcnt, std::vector<cache_miss> *misses) {
	auto iovs = smartiov(iov, iovcnt);
	auto bytes = iovs.bytes();

	lba_t base = offset/512, limit = base + bytes/512, prev = base;
	std::unique_lock<std::mutex> lk(m);

	size_t buf_offset = 0;
	for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, plba] = it->vals(base, limit);
	    if (_base > prev) {
		size_t bytes = 512 * (_base - prev);
		misses->push_back(std::tuple((size_t)512*prev, bytes, buf_offset));
		buf_offset += bytes;
	    }

	    size_t bytes = 512 * (_limit - _base),
		nvme_offset = 512 * plba;
	    auto slice = iovs.slice(buf_offset, buf_offset+bytes);

	    lk.unlock();
	    if (preadv(fd, slice.data(), slice.size(), nvme_offset) < 0)
		throw_fs_error("wcache_read");
	    lk.lock();

	    buf_offset += bytes;
	    prev = _limit;
	}
	if (prev < limit)
	    misses->push_back(std::tuple(512 * prev, 512 * (limit - prev), buf_offset));
    }

    
    void aio_readv(size_t offset, const iovec *iov, int iovcnt, std::vector<cache_miss> *misses, void (*cb)(void*), void *ptr) {
	auto w = new aio_readv_work(offset, iov, iovcnt, misses, cb, ptr);
	readv_workers.put(w);
    }
    
    // debugging
    void getmap(int base, int limit, int (*cb)(void*, int, int, int), void *ptr) {
	for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, plba] = it->vals(base, limit);
	    if (!cb(ptr, (int)_base, (int)_limit, (int)plba))
		break;
	}
    }
    void reset(void) {
	map.reset();
    }
    void get_super(j_write_super *s) {
	*s = *super;
    }
    page_t do_get_oldest(page_t blk, std::vector<j_extent> &extents) {
	return get_oldest(blk, extents);
    }
    void do_write_checkpoint(void) {
	if (map_dirty)
	    write_checkpoint();
    }
};

/* ------------------- DEBUGGING ----------------------*/

translate   *lsvd;
write_cache *wcache;
objmap      *omap;
read_cache  *rcache;
backend     *io;

struct tuple {
    int base;
    int limit;
    int obj;			// object map
    int offset;
    int plba;			// write cache map
};
struct getmap_s {
    int i;
    int max;
    struct tuple *t;
};

extern "C" void wcache_init(uint32_t blkno, int fd)
{
    wcache = new write_cache(blkno, fd, lsvd, 2);
}

extern "C" void wcache_shutdown(void)
{
    delete wcache;
    wcache = NULL;
}

struct waitq {
public:
    std::mutex m;
    std::condition_variable cv;
    bool done;
    waitq(){done = false;}
};

static void waitq_cb(void *ptr)
{
    waitq *q = (waitq*)ptr;
    std::unique_lock<std::mutex> lk(q->m);
    q->done = true;
    q->cv.notify_all();
}

extern "C" void wcache_write(char *buf, uint64_t offset, uint64_t len)
{
    waitq q;
    iovec iov = {buf, len};
    std::unique_lock<std::mutex> lk(q.m);
    wcache->write(offset, &iov, 1, waitq_cb, (void*)&q);
    while (!q.done)
	q.cv.wait(lk);
}

extern "C" void wcache_read(char *buf, uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    iovec iov = {buf2, len};
    std::vector<write_cache::cache_miss> misses;
    wcache->readv(offset, &iov, 1, &misses);

    memcpy(buf, buf2, len);
    free(buf2);
}

static int wc_getmap_cb(void *ptr, int base, int limit, int plba)
{
    getmap_s *s = (getmap_s*)ptr;
    if (s->i < s->max)
	s->t[s->i++] = (tuple){base, limit, 0, 0, plba};
    return s->i < s->max;
}
extern "C" int wcache_getmap(int base, int limit, int max, struct tuple *t)
{
    getmap_s s = {0, max, t};
    wcache->getmap(base, limit, wc_getmap_cb, (void*)&s);
    return s.i;
}

extern "C" void wcache_get_super(j_write_super *s)
{
    wcache->get_super(s);
}

extern "C" int wcache_oldest(int blk, j_extent *extents, int max, int *p_n)
{
    std::vector<j_extent> exts;
    int next_blk = wcache->do_get_oldest(blk, exts);
    int n = std::min(max, (int)exts.size());
    memcpy((void*)extents, exts.data(), n*sizeof(j_extent));
    *p_n = n;
    return next_blk;
}

extern "C" void wcache_write_ckpt(void)
{
    wcache->do_write_checkpoint();
}

extern "C" void wcache_reset(void)
{
    wcache->reset();
}

extern "C" void c_shutdown(void)
{
    lsvd->shutdown();
    delete lsvd;
    delete omap;
    delete io;
}

extern "C" int c_flush(void)
{
    return lsvd->flush();
}

extern "C" ssize_t c_init(char *name, int n, bool flushthread, bool nocache)
{
    io = new file_backend(name);
    omap = new objmap();
    lsvd = new translate(io, omap);
    lsvd->nocache = nocache;
    return lsvd->init(name, n, flushthread);
}

extern "C" int c_size(void)
{
    return lsvd->mapsize();
}

extern "C" int c_read(char *buffer, uint64_t offset, uint32_t size)
{
    size_t val = lsvd->read(offset, size, buffer);
    return val < 0 ? -1 : 0;
}

extern "C" int c_write(char *buffer, uint64_t offset, uint32_t size)
{
    size_t val = lsvd->write(offset, size, buffer);
    return val < 0 ? -1 : 0;
}

extern "C" int dbg_inmem(int max, int *list)
{
    return lsvd->inmem(max, list);
}

static int getmap_cb(void *ptr, int base, int limit, int obj, int offset)
{
    getmap_s *s = (getmap_s*)ptr;
    if (s->i < s->max) 
	s->t[s->i++] = (tuple){base, limit, obj, offset, 0};
    return s->i < s->max;
}
   
extern "C" int dbg_getmap(int base, int limit, int max, struct tuple *t)
{
    getmap_s s = {0, max, t};
    lsvd->getmap(base, limit, getmap_cb, (void*)&s);
    return s.i;
}

extern "C" int dbg_checkpoint(void)
{
    return lsvd->checkpoint();
}

extern "C" void dbg_reset(void)
{
    lsvd->reset();
}

extern "C" int dbg_frontier(void)
{
    return lsvd->frontier();
}

extern "C" void rcache_init(uint32_t blkno, int fd)
{
    rcache = new read_cache(blkno, fd, false, lsvd, omap, io);
}

extern "C" void rcache_shutdown(void)
{
    delete rcache;
    rcache = NULL;
}

extern "C" void rcache_evict(int n)
{
    rcache->do_evict(n);
}

extern "C" void rcache_add(int object, int sector_offset, char* buf, size_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    memcpy(buf2, buf, len);
    extmap::obj_offset oo = {object, sector_offset};
    rcache->add(oo, len/512, buf2);
    free(buf2);
}

extern "C" void rcache_read(char *buf, uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    waitq q;
    iovec iov = {buf2, len};
    std::unique_lock lk(q.m);
    rcache->readv(offset, &iov, 1, waitq_cb, (void*)&q);
    while (!q.done)
	q.cv.wait(lk);
    memcpy(buf, buf2, len);
    free(buf2);
}

extern "C" extern "C" void rcache_getsuper(j_read_super *p_super)
{
    j_read_super *p;
    rcache->get_info(&p, NULL, NULL, NULL, NULL);
    *p_super = *p;
}

extern "C" int rcache_getmap(extmap::obj_offset *keys, int *vals, int n)
{
    int i = 0;
    std::map<extmap::obj_offset,int> *p_map;
    rcache->get_info(NULL, NULL, NULL, NULL, &p_map);
    for (auto it = p_map->begin(); it != p_map->end() && i < n; it++, i++) {
	auto [key, val] = *it;
	keys[i] = key;
	vals[i] = val;
    }
    return i;
}

extern "C" int rcache_get_flat(extmap::obj_offset *vals, int n)
{
    extmap::obj_offset *p;
    j_read_super *p_super;
    rcache->get_info(&p_super, &p, NULL, NULL, NULL);
    n = std::min(n, p_super->units);
    memcpy(vals, p, n*sizeof(extmap::obj_offset));
    return n;
}

extern "C" int rcache_get_masks(uint16_t *vals, int n)
{
    j_read_super *p_super;
    uint16_t *masks;
    rcache->get_info(&p_super, NULL, &masks, NULL, NULL);
    n = std::min(n, p_super->units);
    memcpy(vals, masks, n*sizeof(uint16_t));
    return n;
}

extern "C" void rcache_reset(void)
{
}

extern "C" void fakemap_update(int base, int limit, int obj, int offset)
{
    extmap::obj_offset oo = {obj,offset};
    omap->map.update(base, limit, oo);
}

extern "C" void fakemap_reset(void)
{
    omap->map.reset();
}

/* ------------------- FAKE RBD INTERFACE ----------------------*/
/* following types are from librados.h
 */
enum {
    EVENT_TYPE_PIPE = 1,
    EVENT_TYPE_EVENTFD = 2
};
    
typedef void *rbd_image_t;
typedef void *rbd_image_options_t;
typedef void *rbd_pool_stats_t;

typedef void *rbd_completion_t;
typedef void (*rbd_callback_t)(rbd_completion_t cb, void *arg);

typedef void *rados_ioctx_t;
typedef void *rados_t;
typedef void *rados_config_t;

#define RBD_MAX_BLOCK_NAME_SIZE 24
#define RBD_MAX_IMAGE_NAME_SIZE 96

/* fio only looks at 'size' */
typedef struct {
  uint64_t size;
  uint64_t obj_size;
  uint64_t num_objs;
  int order;
  char block_name_prefix[RBD_MAX_BLOCK_NAME_SIZE]; /* deprecated */
  int64_t parent_pool;                             /* deprecated */
  char parent_name[RBD_MAX_IMAGE_NAME_SIZE];       /* deprecated */
} rbd_image_info_t;

typedef struct {
  uint64_t id;
  uint64_t size;
  const char *name;
} rbd_snap_info_t;

/* now our fake implementation
 */
struct fake_rbd_image {
    std::mutex   m;
    backend     *io;
    objmap      *omap;
    translate   *lsvd;
    write_cache *wcache;
    read_cache  *rcache;
    ssize_t      size;		// bytes
    int          fd;		// cache device
    j_super     *js;		// cache page 0
    bool         notify;
    int          eventfd;
    std::queue<rbd_completion_t> completions;
};

struct lsvd_completion {
public:
    fake_rbd_image *fri;
    rbd_callback_t cb;
    void *arg;
    int retval;
    bool done;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<int> refcount;

    lsvd_completion() : done(false), refcount(0) {}
    void get(void) {
	refcount++;
    }
    void put(void) {
	if (--refcount == 0)
	    delete this;
    }
    
    void complete(int val) {
	retval = val;
	std::unique_lock lk(m);
	done = true;
	cb((rbd_completion_t)this, arg);
	if (fri->notify) {
	    fri->completions.push((rbd_completion_t)this);
	    uint64_t value = 1;
	    if (write(fri->eventfd, &value, sizeof (value)) < 0)
		throw_fs_error("eventfd");
	}
	cv.notify_all();
	lk.unlock();
    }
};

extern "C" int rbd_poll_io_events(rbd_image_t image, rbd_completion_t *comps, int numcomp)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    std::unique_lock lk(fri->m);
    int i;
    for (i = 0; i < numcomp && !fri->completions.empty(); i++) {
	comps[i] = fri->completions.front();
	fri->completions.pop();
    }
    return i;
}

extern "C" int rbd_set_image_notification(rbd_image_t image, int fd, int type)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    assert(type == EVENT_TYPE_EVENTFD);
    fri->notify = true;
    fri->eventfd = fd;
    return 0;
}

extern "C" int rbd_aio_create_completion(void *cb_arg,
					 rbd_callback_t complete_cb, rbd_completion_t *c)
{
    lsvd_completion *p = new lsvd_completion;
    p->cb = complete_cb;
    p->arg = cb_arg;
    p->refcount = 1;
    *c = (rbd_completion_t)p;
    DBG((long)p);
    return 0;
}

extern "C" void rbd_aio_release(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->put();
}

extern "C" int rbd_aio_discard(rbd_image_t image, uint64_t off, uint64_t len, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = (fake_rbd_image*)image;
    p->complete(0);
    return 0;
}

extern "C" int rbd_aio_flush(rbd_image_t image, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = (fake_rbd_image*)image;
    p->complete(0);
    return 0;
}

extern "C" int rbd_flush(rbd_image_t image)
{
    auto fri = (fake_rbd_image*)image;
    fri->lsvd->flush();
    fri->lsvd->checkpoint();
    return 0;
}

extern "C" void *rbd_aio_get_arg(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    return p->arg;
}

extern "C" ssize_t rbd_aio_get_return_value(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    return p->retval;
}

struct readv_state {
    int phase;
    rbd_image_t image;
    const iovec *iov;
    int iovcnt;
    uint64_t off;
    rbd_completion_t c;
    
    fake_rbd_image *fri;
    lsvd_completion *p;
    char *aligned_buf;
    smartiov iovs;
    iovec tmp;
    const iovec *tmp_iov;
    int    tmp_iovcnt;
    std::vector<write_cache::cache_miss> misses;
    std::atomic<int> n;
    std::mutex m;		// to make valgrind happy

    readv_state(rbd_image_t _image, const iovec *_iov, int _iovcnt,
		uint64_t _off, rbd_completion_t _c) :
	phase(0), image(_image), iov(_iov), iovcnt(_iovcnt), off(_off),
	c(_c), n(0) {}
    ~readv_state(){ std::unique_lock lk(m);}
};

void rbd_aio_readv_fsm(void *ptr)
{
    readv_state *s = (readv_state*)ptr;
    std::unique_lock lk(m);

    switch (s->phase) {
    case 0:
	s->fri = (fake_rbd_image*)s->image;
	s->p = (lsvd_completion*)s->c;
	s->p->fri = s->fri;
	s->p->get();

	s->aligned_buf = NULL;
	s->iovs.ingest(s->iov, s->iovcnt);
	s->tmp_iov = s->iovs.data();
	s->tmp_iovcnt = s->iovs.size();

	if (!s->iovs.aligned(512)) {
	    s->aligned_buf = (char*)aligned_alloc(512, s->iovs.bytes());
	    s->iovs.copy_out(s->aligned_buf);
	    s->tmp = (iovec){s->aligned_buf, s->iovs.bytes()};
	    s->tmp_iov = &s->tmp;
	    s->tmp_iovcnt = 1;
	}
	s->phase = 1;
	lk.unlock();
	s->fri->wcache->aio_readv(s->off, s->tmp_iov, s->tmp_iovcnt,
				  &s->misses, rbd_aio_readv_fsm, ptr);
	break;
	
    case 1:
	if (s->misses.size() == 0) {
	    s->phase = 3;
	    lk.unlock();
	    rbd_aio_readv_fsm(ptr);
	}
	else {
	    auto iovs2 = smartiov(s->tmp_iov, s->tmp_iovcnt);
	    s->phase = 2;
	    lk.unlock();
	    for (auto [_off, _len, buf_offset] : s->misses) {
		auto slice = iovs2.slice(buf_offset, buf_offset+_len);
		s->n++;
		s->fri->rcache->readv(_off, slice.data(), slice.size(),
				      rbd_aio_readv_fsm, ptr);
	    }
	}
	break;
	
    case 2:
	if (--(s->n) == 0) {
	    s->phase = 3;
	    lk.unlock();
	    rbd_aio_readv_fsm(ptr);
	}
	break;
       
    case 3: {
	if (s->aligned_buf) {
	    s->iovs.copy_in(s->aligned_buf);
	    free(s->aligned_buf);
	}
	s->p->complete(0);
	s->p->put();
	delete s;
    } break;
	
    default:
	assert(false);
    }
}

extern "C" int rbd_aio_readv(rbd_image_t image, const iovec *iov,
			     int iovcnt, uint64_t off, rbd_completion_t c)
{
    auto s = new readv_state(image, iov, iovcnt, off, c);
    rbd_aio_readv_fsm((void*)s);
    return 0;
}

extern "C" int rbd_aio_read(rbd_image_t image, uint64_t off, size_t len, const char *buf,
			    rbd_completion_t c)
{
    iovec iov = {(char*)buf, len};
    return rbd_aio_readv(image, &iov, 1, off, c);
}

static void fake_rbd_cb(void *ptr)
{
    lsvd_completion *p = (lsvd_completion *)ptr;
    p->get();
    p->complete(0);
    p->put();
}

extern "C" int rbd_aio_writev(rbd_image_t image, const struct iovec *iov,
			      int iovcnt, uint64_t off, rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = fri;
    fri->wcache->write(off, iov, iovcnt, fake_rbd_cb, (void*)c);
    return 0;
}

extern "C" int rbd_aio_write(rbd_image_t image, uint64_t off, size_t len, const char *buf,
			     rbd_completion_t c)
{
    iovec iov = {(void*)buf, len};
    return rbd_aio_writev(image, &iov, 1, off, c);
}

static void fake_rbd_cb2(rbd_completion_t c, void *arg)
{
    waitq *q = (waitq*)arg;
    std::unique_lock lk(q->m);
    q->done = true;
    q->cv.notify_all();
}

typedef int (rbd_op_t)(rbd_image_t, uint64_t, size_t, const char*, rbd_completion_t);

int rbd_op(rbd_op_t op, rbd_image_t image, uint64_t off, size_t len, char *buf)
{
    rbd_completion_t c;
    waitq q;
    rbd_aio_create_completion((void*)&q, fake_rbd_cb2, &c);
    op(image, off, len, buf, c);
    std::unique_lock lk(q.m);
    while (!q.done)
	q.cv.wait(lk);
    auto val = rbd_aio_get_return_value(c);
    rbd_aio_release(c);
    return val;
}

extern "C" int rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf)
{
    return rbd_op(rbd_aio_read, image, off, len, buf);
}

extern "C" int rbd_write(rbd_image_t image, uint64_t off, size_t len, char *buf)
{
    return rbd_op(rbd_aio_write, image, off, len, buf);
}

extern "C" int rbd_aio_wait_for_complete(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    std::unique_lock lk(p->m);
    p->get();
    while (!p->done)
	p->cv.wait(lk);
    p->put();
    return 0;
}

extern "C" int rbd_stat(rbd_image_t image, rbd_image_info_t *info, size_t infosize)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    info->size = fri->size;
    return 0;
}

extern "C" int rbd_get_size(rbd_image_t image, uint64_t *size)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    *size = fri->size;
    return 0;
}

static std::pair<std::string,std::string> split_string(std::string s, std::string delim)
{
    auto i = s.find(delim);
    return std::pair(s.substr(0,i), s.substr(i+delim.length()));
}

static void* delay_printlog(void *q)
{
    sleep(50);
    FILE *fp = fopen("/tmp/print.log", "w");
    printlog(fp);
    fclose(fp);
    exit(0);
}
pthread_t _th;

extern "C" int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image,
			const char *snap_name)
{
//    logbuf = logptr = (struct _log*)malloc(2*1024*1024*8*sizeof(*logbuf));
//    pthread_create(&_th, NULL, delay_printlog, NULL);

    int rv;
    auto [nvme, obj] = split_string(std::string(name), ",");
    auto fri = new fake_rbd_image;

    // c_init:
    fri->io = new file_backend(obj.c_str());
    fri->omap = new objmap();
    fri->lsvd = new translate(fri->io, fri->omap);
    int n_xlate_threads = 3;
    char *nxt = getenv("N_XLATE");
    if (nxt) {
	n_xlate_threads = atoi(nxt);
	printf("translate threads: %d\n", n_xlate_threads);
    }
    fri->size = fri->lsvd->init(obj.c_str(), n_xlate_threads, true);
    
    int fd = fri->fd = open(nvme.c_str(), O_RDWR | O_DIRECT);
    j_super *js = fri->js = (j_super*)aligned_alloc(512, 4096);
    if ((rv = pread(fd, (char*)js, 4096, 0)) < 0)
	return rv;
    if (js->magic != LSVD_MAGIC || js->type != LSVD_J_SUPER)
	return -1;

    int n_wc_threads = 2;
    char *nwt = getenv("N_WCACHE");
    if (nwt) {
	n_wc_threads = atoi(nwt);
	printf("write cache threads: %d\n", n_wc_threads);
    }
    
    fri->wcache = new write_cache(js->write_super, fd, fri->lsvd, n_wc_threads);
    fri->rcache = new read_cache(js->read_super, fd, false, fri->lsvd, fri->omap, fri->io);
    fri->notify = false;
    
    *image = (void*)fri;
    return 0;
}

extern "C" int rbd_close(rbd_image_t image)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    fri->rcache->write_map();
    delete fri->rcache;
    fri->wcache->do_write_checkpoint();
    delete fri->wcache;
    close(fri->fd);
    fri->lsvd->flush();
    fri->lsvd->checkpoint();
    delete fri->lsvd;
    delete fri->omap;
    delete fri->io;
    
    return 0;
}

fake_rbd_image _fri;

extern "C" void fake_rbd_init(void)
{
    _fri.io = io;
    _fri.omap = omap;
    _fri.lsvd = lsvd;
    _fri.size = 0;
    _fri.wcache = wcache;
    _fri.rcache = rcache;
}

extern "C" void fake_rbd_read(char *buf, size_t off, size_t len)
{
    rbd_completion_t c;
    waitq q;
    rbd_aio_create_completion((void*)&q, fake_rbd_cb2, &c);
    rbd_aio_read((rbd_image_t)&_fri, off, len, buf, c);
    std::unique_lock lk(q.m);
    while (!q.done)
	q.cv.wait(lk);
    rbd_aio_release(c);
}

extern "C" void fake_rbd_write(char *buf, size_t off, size_t len)
{
    rbd_completion_t c;
    waitq q;
    rbd_aio_create_completion((void*)&q, fake_rbd_cb2, &c);
    rbd_aio_write((rbd_image_t)&_fri, off, len, buf, c);
    std::unique_lock lk(q.m);
    while (!q.done)
	q.cv.wait(lk);
    rbd_aio_release(c);
}

/* any following functions are stubs only
 */
extern "C" int rbd_invalidate_cache(rbd_image_t image)
{
    return 0;
}

/* we just need null implementations of the RADOS functions.
 */
extern "C" int rados_conf_read_file(rados_t cluster, const char *path)
{
    return 0;
}

extern "C" int rados_conf_set(rados_t cluster, const char *option, const char *value)
{
    return 0;
}

extern "C" int rados_connect(rados_t cluster)
{
    return 0;
}

extern "C" int rados_create(rados_t *cluster, const char * const id)
{
    return 0;
}

extern "C" int rados_create2(rados_t *pcluster, const char *const clustername,
			     const char * const name, uint64_t flags)
{
    return 0;
}

extern "C" int rados_ioctx_create(rados_t cluster, const char *pool_name, rados_ioctx_t *ioctx)
{
    return 0;
}

extern "C" void rados_ioctx_destroy(rados_ioctx_t io)
{
}

extern "C" void rados_shutdown(rados_t cluster)
{
}

/* These RBD functions are unimplemented and return errors
 */

extern "C" int rbd_create(rados_ioctx_t io, const char *name, uint64_t size,
                            int *order)
{
    return -1;
}
extern "C" int rbd_resize(rbd_image_t image, uint64_t size)
{
    return -1;
}

extern "C" int rbd_snap_create(rbd_image_t image, const char *snapname)
{
    return -1;
}
extern "C" int rbd_snap_list(rbd_image_t image, rbd_snap_info_t *snaps,
                               int *max_snaps)
{
    return -1;
}
extern "C" void rbd_snap_list_end(rbd_snap_info_t *snaps)
{
}
extern "C" int rbd_snap_remove(rbd_image_t image, const char *snapname)
{
    return -1;
}
extern "C" int rbd_snap_rollback(rbd_image_t image, const char *snapname)
{
    return -1;
}
