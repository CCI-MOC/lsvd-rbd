/*
 * file:        first-try.cc
 * description: first pass at a userspace block-on-object layer
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

#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

typedef int64_t lba_t;

/* make this atomic? */
int batch_seq;
int last_ckpt;
const int BATCH_SIZE = 8 * 1024 * 1024;
uuid_t my_uuid;

static int div_round_up(int n, int m)
{
    return (n + m - 1) / m;
}

size_t iov_sum(iovec *iov, int iovcnt)
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
	running = false;
	cv.notify_all();
	while (!pool.empty()) {
	    pool.front().join();
	    pool.pop();
	}
    }	
    T get_locked(std::unique_lock<std::mutex> &lk) {
	while (running && q.empty())
	    cv.wait(lk);
	auto val = q.front();
	q.pop();
	return val;
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
    T get(void) {
	std::unique_lock<std::mutex> lk(*m);
	return get_locked();
    }
    void put_locked(T work, std::unique_lock<std::mutex> &lk) {
	q.push(work);
	cv.notify_one();
    }
    void put(T work) {
	std::unique_lock<std::mutex> lk(*m);
	put_locked(work, lk);
    }
};

class objmap {
    std::shared_mutex  sm;
    extmap::objmap      object_map;
};

class translate {
    std::mutex          m;
    extmap::objmap      object_map;

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

    char      *super;
    hdr       *super_h;
    super_hdr *super_sh;
    size_t     super_len;

    thread_pool<batch*> workers;
    thread_pool<int>    misc_threads;
    
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

    /* these all should probably be combined with the stuff in objects.cc to create
     * object classes that serialize and de-serialize themselves. Sometime, maybe.
     */
    template <class T>
    void decode_offset_len(char *buf, size_t offset, size_t len, std::vector<T> &vals) {
	T *p = (T*)(buf + offset), *end = (T*)(buf + offset + len);
	for (; p < end; p++)
	    vals.push_back(*p);
    }

    /* clone_info is variable-length, so we need to pass back pointers 
     * rather than values. That's OK because we allocate superblock permanently
     */
    typedef clone_info *clone_p;
    ssize_t read_super(char *name, std::vector<uint32_t> &ckpts,
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

    /* TODO: object list
     */
    int write_checkpoint(int seq) {
	std::vector<ckpt_mapentry> entries;
	std::vector<ckpt_obj> objects;
	
	std::unique_lock<std::mutex> lk(m);
	last_ckpt = seq;
	for (auto it = object_map.begin(); it != object_map.end(); it++) {
	    auto [base, limit, ptr] = it->vals();
	    entries.push_back((ckpt_mapentry){.lba = base, .len = limit-base,
			.obj = (int32_t)ptr.obj, .offset = (int32_t)ptr.offset});
	}
	size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);

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
	lk.unlock();

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
	return seq;
    }
    
    int make_hdr(char *buf, batch *b) {
	hdr *h = (hdr*)buf;
	*h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0}, .type = LSVD_DATA,
		   .seq = (uint32_t)b->seq, .hdr_sectors = 8, .data_sectors = (uint32_t)(b->len / 512)};
	memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));

	data_hdr *dh = (data_hdr*)(h+1);
	uint32_t o1 = sizeof(*h) + sizeof(*dh), l1 = sizeof(uint32_t), o2 = o1 + l1,
	    l2 = b->entries.size() * sizeof(data_map);
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
	    std::unique_lock<std::mutex> lk(*p->m);
	    auto b = p->get_locked(lk);
	    if (!p->running)
		return;


	    uint32_t sectors = div_round_up(b->hdrlen(), 512);
	    object_info[b->seq] = (obj_info){.hdr = sectors, .data = (uint32_t)(b->len / 512),
					     .live = (uint32_t)(b->len / 512), .type = LSVD_DATA};
	    lk.unlock();

	    char *hdr = (char*)calloc(sectors*512, 1);
	    make_hdr(hdr, b);
	    iovec iov[2] = {{hdr, (size_t)(sectors*512)}, {b->buf, b->len}};
	    io->write_numbered_object(b->seq, iov, 2);
	    free(hdr);

	    lk.lock();
	    in_mem_objects.erase(b->seq);
	    batches.push(b);
	    lk.unlock();
	}
    }

    void ckpt_thread(thread_pool<int> *p) {
	auto one_second = std::chrono::seconds(1);
	auto seq0 = batch_seq;
	const int ckpt_interval = 100;

	while (p->running) {
	    std::unique_lock<std::mutex> lk(*p->m);
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

    translate(backend *_io) : workers(&m), misc_threads(&m) {
	io = _io;
	current_batch = NULL;
    }
    ~translate() {
	while (!batches.empty()) {
	    auto b = batches.top();
	    batches.pop();
	    delete b;
	}
	if (current_batch)
	    delete current_batch;
	free(super);
	delete io;
    }
    
    /* returns sequence number of ckpt
     */
    int checkpoint(void) {
	std::unique_lock<std::mutex> lk(m);
	if (current_batch && current_batch->len > 0) {
	    workers.put_locked(current_batch, lk);
	    current_batch = NULL;
	}
	int seq = batch_seq++;
	lk.unlock();
	return write_checkpoint(seq);
    }

    int flush(void) {
	std::unique_lock<std::mutex> lock(m);
	int val = 0;
	if (current_batch && current_batch->len > 0) {
	    val = current_batch->seq;
	    workers.put_locked(current_batch, lock);
	    current_batch = NULL;
	}
	return val;
    }

    ssize_t init(char *name, int nthreads, bool timedflush) {
	std::vector<uint32_t>  ckpts;
	std::vector<clone_p>   clones;
	std::vector<snap_info> snaps;
	ssize_t bytes = read_super(name, ckpts, clones, snaps);
	if (bytes < 0)
	  return bytes;
	batch_seq = super_sh->next_obj;

	int _ckpt = 1;
	for (auto ck : ckpts) {
	    ckpts.resize(0);
	    std::vector<ckpt_obj> objects;
	    std::vector<deferred_delete> deletes;
	    std::vector<ckpt_mapentry> map;
	    if (read_checkpoint(ck, ckpts, objects, deletes, map) < 0)
		return -1;
	    for (auto o : objects) {
		object_info[o.seq] = (obj_info){.hdr = o.hdr_sectors, .data = o.data_sectors,
					.live = o.live_sectors, .type = LSVD_DATA};

	    }
	    for (auto m : map) {
		object_map.update(m.lba, m.lba + m.len,
				  (extmap::obj_offset){.obj = m.obj,
					  .offset = m.offset});
	    }
	    _ckpt = ck;
	}

	for (int i = _ckpt; ; i++) {
	    std::vector<uint32_t>    ckpts;
	    std::vector<obj_cleaned> cleaned;
	    std::vector<data_map>    map;
	    hdr h; data_hdr dh;
	    batch_seq = i;
	    if (read_data_hdr(i, h, dh, ckpts, cleaned, map) < 0)
		break;
	    object_info[i] = (obj_info){.hdr = h.hdr_sectors, .data = h.data_sectors,
					.live = h.data_sectors, .type = LSVD_DATA};
	    int offset = 0;
	    for (auto m : map) {
		object_map.update(m.lba, m.lba + m.len,
				  (extmap::obj_offset){.obj = i, .offset = offset});
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
	std::unique_lock<std::mutex> lock(m);
	size_t len = iov_sum(iov, iovcnt);

	if (current_batch && current_batch->len + len > current_batch->max) {
	    workers.put_locked(current_batch, lock);
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
	    in_mem_objects[current_batch->seq] = current_batch->buf;
	}

	int64_t sector_offset = current_batch->len / 512,
	    lba = offset/512, limit = (offset+len)/512;
	current_batch->append_iov(offset / 512, iov, iovcnt);

	std::vector<extmap::lba2obj> deleted;
	extmap::obj_offset oo = {current_batch->seq, sector_offset};
	object_map.update(lba, limit, oo, &deleted);

	for (auto d : deleted) {
	    auto [base, limit, ptr] = d.vals();
	    object_info[ptr.obj].live -= (limit - base);
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

	if (object_map.size() == 0) {
	    memset(buf, 0, len);
	    return len;
	}

	/* object number, offset (bytes), length (bytes) */
	std::vector<std::tuple<int, size_t, size_t>> regions;
	std::unique_lock<std::mutex> lock(m);

	auto prev = base;
	char *ptr = buf;
	for (auto it = object_map.lookup(base); it != object_map.end() && it->base() < limit; it++) {
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
	lock.unlock();

	ptr = buf;
	for (auto [obj, _offset, _len] : regions) {
	    if (obj == -1)
		memset(ptr, 0, _len);
	    else if (obj == -2)
		/* skip */;
	    else
		io->read_numbered_object(obj, ptr, _offset + object_info[obj].hdr*512, _len);
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
	for (auto it = object_map.lookup(base); it != object_map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, oo] = it->vals(base, limit);
	    if (!cb(ptr, (int)_base, (int)_limit, (int)oo.obj, (int)oo.offset))
		break;
	}
    }
    int mapsize(void) {
	return object_map.size();
    }
    void reset(void) {
	object_map.reset();
    }
    int frontier(void) {
	if (current_batch)
	    return current_batch->len / 512;
	return 0;
    }
};


/* simple backend that uses files in a directory. 
 * good for debugging and testing
 */
class file_backend : public backend {
    char *prefix;
public:
    file_backend(char *_prefix) {
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


/* each write queues up one of these for a worker thread
 */
struct wcache_work {
    uint64_t  lba;
    iovec    *iov;
    int       iovcnt;
    void    (*callback)(void*);
    void     *ptr;
};

static bool aligned(void *ptr, int a)
{
    return 0 == ((long)ptr & (a-1));
}

static void iov_consume(iovec *iov, int iovcnt, size_t nbytes)
{
    for (int i = 0; i < iovcnt && nbytes > 0; i++) {
	if (iov[i].iov_len < nbytes) {
	    nbytes -= iov[i].iov_len;
	    iov[i].iov_len = 0;
	}
	else {
	    iov[i].iov_len -= nbytes;
	    iov[i].iov_base = (char*)iov[i].iov_base + nbytes;
	    nbytes = 0;
	}
    }
}

static void iov_zero(iovec *iov, int iovcnt, size_t nbytes)
{
    for (int i = 0; i < iovcnt && nbytes > 0; i++) {
	if (iov[i].iov_len < nbytes) {
	    memset(iov[i].iov_base, 0, iov[i].iov_len);
	    nbytes -= iov[i].iov_len;
	    iov[i].iov_len = 0;
	}
	else {
	    memset(iov[i].iov_base, 0, iov[i].iov_len);
	    iov[i].iov_len -= nbytes;
	    iov[i].iov_base = (char*)iov[i].iov_base + nbytes;
	    nbytes = 0;
	}
    }
}


/* all addresses are in units of 4KB blocks
 * TODO: should this use direct I/O? 
 */
class write_cache {
    int            fd;
    uint32_t       super_blkno;
    j_write_super *super;	// 4KB

    extmap::cachemap2 map;
    translate        *be;
    
    uint32_t       base;
    uint32_t       limit;
    uint32_t       next;
    uint32_t       oldest;

    thread_pool<wcache_work> workers;
    std::mutex               m;

    uint64_t sequence;
    char *pad_page;
    
    static const int n_threads = 1;

    /* lock must be held before calling
     */
    uint32_t allocate(uint32_t n, uint32_t &pad) {
	pad = 0;
	if (limit - next < n) {
	    pad = next;
	    next = 0;
	}
	auto val = next;
	next += n;
	return val;
    }

    j_hdr *mk_header(char *buf, uint32_t type, uuid_t &uuid, uint32_t blks) {
	memset(buf, 0, 4096);
	j_hdr *h = (j_hdr*)buf;
	*h = (j_hdr){.magic = LSVD_MAGIC, .type = type, .version = 1, .vol_uuid = {0},
		     .seq = sequence++, .len = blks, .crc32 = 0, .extent_offset = 0, .extent_len = 0};
	memcpy(h->vol_uuid, uuid, sizeof(uuid));
	return h;
    }

    void writer(thread_pool<wcache_work> *p) {
	char *buf = (char*)aligned_alloc(512, 4096); // for direct I/O
	
	while (p->running) {
	    std::vector<char*> bounce_bufs;
	    std::unique_lock<std::mutex> lk(m);
	    if (!p->wait_locked(lk))
		break;

	    std::vector<wcache_work> work;
	    std::vector<int> lengths;
	    int sectors = 0;
	    while (p->running) {
		wcache_work w;
		if (!p->get_nowait(w))
		    break;
		auto l = iov_sum(w.iov, w.iovcnt) / 512;		
		sectors += l;
		lengths.push_back(l);
		work.push_back(w);
	    }
	    
	    int blocks = div_round_up(sectors, 8);
	    // allocate blocks + 1
	    uint32_t pad, blockno = allocate(blocks+1, pad);
	    lk.unlock();

	    if (pad != 0) {
		mk_header(buf, LSVD_J_PAD, my_uuid, (limit - pad));
		if (pwrite(fd, buf, 4096, pad*4096) < 0)
		    /* TODO: do something */;
	    }

	    std::vector<j_extent> extents;
	    for (auto w : work) {
		for (int i = 0; i < w.iovcnt; i++) {
		    if (aligned(w.iov[i].iov_base, 512))
			continue;
		    char *p = (char*)aligned_alloc(512, w.iov[i].iov_len);
		    memcpy(p, w.iov[i].iov_base, w.iov[i].iov_len);
		    w.iov[i].iov_base = p;
		    bounce_bufs.push_back(p);
		}
		extents.push_back((j_extent){w.lba, iov_sum(w.iov, w.iovcnt)/512});
	    }

	    j_hdr *j = mk_header(buf, LSVD_J_DATA, my_uuid, 1+blocks);
	    j->extent_offset = sizeof(*j);
	    size_t e_bytes = extents.size() * sizeof(j_extent);
	    j->extent_len = e_bytes;
	    memcpy((void*)(buf + sizeof(*j)), (void*)extents.data(), e_bytes);
		
	    std::vector<iovec> iovs;
	    iovs.push_back((iovec){buf, 4096});

	    for (auto w : work)
		for (int i = 0; i < w.iovcnt; i++)
		    iovs.push_back(w.iov[i]);
		    
	    size_t pad_sectors = blocks*8 - sectors;
	    if (pad_sectors > 0)
		iovs.push_back((iovec){pad_page, pad_sectors*512});

	    if (pwritev(fd, iovs.data(), iovs.size(), blockno*4096) < 0)
		/* TODO: do something */;

	    /* update map first under lock. 
	     * Note that map is in units of *sectors*, not blocks 
	     */
	    lk.lock();
	    uint64_t lba = (blockno+1) * 8;
	    for (auto w : work) {
		auto sectors = iov_sum(w.iov, w.iovcnt) / 512;
		map.update(w.lba, w.lba + sectors, lba);
		lba += sectors;
	    }
	    
	    /* then send to backend */
	    lk.unlock();
	    for (auto w : work) {
		be->writev(w.lba*512, w.iov, w.iovcnt);
		w.callback(w.ptr);
	    }

	    while (!bounce_bufs.empty()) {
		free(bounce_bufs.back());
		bounce_bufs.pop_back();
	    }
	}
	free(buf);
    }
    
public:
    write_cache(uint32_t blkno, int _fd, translate *_be) : workers(&m) {
	super_blkno = blkno;
	fd = _fd;
	be = _be;
	char *buf = (char*)aligned_alloc(512, 4096);
	if (pread(fd, buf, 4096, 4096*blkno) < 4096)
	    throw fs::filesystem_error("wcache", std::error_code(errno, std::system_category()));
	super = (j_write_super*)buf;
	pad_page = (char*)aligned_alloc(512, 4096);
	memset(pad_page, 0, 4096);

	base = super->base;
	limit = super->limit;
	next = super->next;
	oldest = super->oldest;
	sequence = super->seq;
	
	// https://stackoverflow.com/questions/22657770/using-c-11-multithreading-on-non-static-member-function
	for (auto i = 0; i < n_threads; i++)
	    workers.pool.push(std::thread(&write_cache::writer, this, &workers));
    }
    ~write_cache() {
	free(pad_page);
	free(super);
    }

    void write(size_t offset, iovec *iov, int iovcnt, void (*callback)(void*), void *ptr) {
	workers.put((wcache_work){offset/512, iov, iovcnt, callback, ptr});
    }

    // TODO: how the hell to handle fragments?
    void readv(size_t offset, iovec *iov, int iovcnt) {
	auto bytes = iov_sum(iov, iovcnt);
	lba_t base = offset/512, limit = base + bytes/512, prev = base;
	std::unique_lock<std::mutex> lk(m);
	
	for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, plba] = it->vals(base, limit);
	    if (_base > prev) {
		size_t bytes = 512 * (_base - prev);
		iov_zero(iov, iovcnt, bytes);
		offset += bytes;
	    }
	    size_t bytes = 512 * (_limit - _base),
		nvme_offset = 512 * plba;
	    lk.unlock();
	    if (preadv(fd, iov, iovcnt, nvme_offset) < 0)
		throw fs::filesystem_error("wcache_read",
					   std::error_code(errno, std::system_category()));
	    lk.lock();
	    iov_consume(iov, iovcnt, bytes);
	    prev = _limit;
	}
	if (prev < limit) 
	    iov_zero(iov, iovcnt, 512 * (limit - prev));

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
};


translate   *lsvd;
write_cache *wcache;

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

extern "C" void wcache_init(uint32_t blkno, int fd);
void wcache_init(uint32_t blkno, int fd)
{
    wcache = new write_cache(blkno, fd, lsvd);
}

extern "C" void wcache_shutdown(void);
void wcache_shutdown(void)
{
    delete wcache;
    wcache = NULL;
}

extern "C" void wcache_write(char* buf, uint64_t offset, uint64_t len);
struct do_write {
public:
    std::mutex m;
    std::condition_variable cv;
    bool done;
    do_write(){done = false;}
};
static void cb(void *ptr)
{
    do_write *d = (do_write*)ptr;
    std::unique_lock<std::mutex> lk(d->m);
    d->done = true;
    d->cv.notify_all();
}
void wcache_write(char *buf, uint64_t offset, uint64_t len)
{
    do_write dw;
    iovec iov = {buf, len};
    std::unique_lock<std::mutex> lk(dw.m);
    wcache->write(offset, &iov, 1, cb, (void*)&dw);
    while (!dw.done)
	dw.cv.wait(lk);
}

extern "C" void wcache_read(char *buf, uint64_t offset, uint64_t len);
void wcache_read(char *buf, uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    iovec iov = {buf2, len};
    wcache->readv(offset, &iov, 1);
    memcpy(buf, buf2, len);
    free(buf2);
}

extern "C" int wcache_getmap(int, int, int, struct tuple*);
int wc_getmap_cb(void *ptr, int base, int limit, int plba)
{
    getmap_s *s = (getmap_s*)ptr;
    if (s->i < s->max)
	s->t[s->i++] = (tuple){base, limit, 0, 0, plba};
    return s->i < s->max;
}
int wcache_getmap(int base, int limit, int max, struct tuple *t)
{
    getmap_s s = {0, max, t};
    wcache->getmap(base, limit, wc_getmap_cb, (void*)&s);
    return s.i;
}

extern "C" void wcache_reset(void);
void wcache_reset(void)
{
    wcache->reset();
}

extern "C" void c_shutdown(void);
void c_shutdown(void)
{
    lsvd->shutdown();
    delete lsvd;
}

extern "C" int c_flush(void);
int c_flush(void)
{
    return lsvd->flush();
}

extern "C" ssize_t c_init(char* name, int n, bool flushthread);
ssize_t c_init(char *name, int n, bool flushthread)
{
    auto io = new file_backend(name);
    lsvd = new translate(io);
    return lsvd->init(name, n, flushthread);
}

extern "C" int c_size(void);
int c_size(void)
{
    return lsvd->mapsize();
}

extern "C" int c_read(char*, uint64_t, uint32_t);
int c_read(char *buffer, uint64_t offset, uint32_t size)
{
    size_t val = lsvd->read(offset, size, buffer);
    return val < 0 ? -1 : 0;
}

extern "C" int c_write(char*, uint64_t, uint32_t);
int c_write(char *buffer, uint64_t offset, uint32_t size)
{
    size_t val = lsvd->write(offset, size, buffer);
    return val < 0 ? -1 : 0;
}

extern "C" int dbg_inmem(int max, int *list);

int dbg_inmem(int max, int *list)
{
    return lsvd->inmem(max, list);
}

int getmap_cb(void *ptr, int base, int limit, int obj, int offset)
{
    getmap_s *s = (getmap_s*)ptr;
    if (s->i < s->max) 
	s->t[s->i++] = (tuple){base, limit, obj, offset, 0};
    return s->i < s->max;
}
   
extern "C" int dbg_getmap(int, int, int, struct tuple*);
int dbg_getmap(int base, int limit, int max, struct tuple *t)
{
    getmap_s s = {0, max, t};
    lsvd->getmap(base, limit, getmap_cb, (void*)&s);
    return s.i;
}

extern "C" int dbg_checkpoint(void);
int dbg_checkpoint(void)
{
    return lsvd->checkpoint();
}

extern "C" void dbg_reset(void);
void dbg_reset(void)
{
    lsvd->reset();
}

extern "C" int dbg_frontier(void);
int dbg_frontier(void)
{
    return lsvd->frontier();
}
