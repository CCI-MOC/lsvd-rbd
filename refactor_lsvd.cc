/*
 * file:        lsvd_rbd.cc
 * description: userspace block-on-object layer with librbd interface
 * 
 * Copyright 2021, 2022 Peter Desnoyers
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "lsvd_includes.h"
#include "base_functions.h"
#include "translate.h"

class backend {
public:
    virtual ssize_t write_object(const char *name, iovec *iov, int iovcnt) = 0;
    virtual ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) = 0;
    virtual ssize_t read_object(const char *name, char *buf, size_t len, size_t offset) = 0;
    virtual ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt,
					  size_t offset) = 0;
    virtual void    delete_numbered_object(int seq) = 0;
    virtual ssize_t read_numbered_object(int seq, char *buf, size_t len,
					 size_t offset) = 0;
    virtual int aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
				    void (*cb)(void*), void *ptr) = 0;
    virtual int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
					  void (*cb)(void*), void *ptr) = 0;
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
    smartiov  iovs;
    cache_work(lba_t _lba, const iovec *iov, int iovcnt,
	       void (*_callback)(void*), void *_ptr) : iovs(iov, iovcnt) {
	lba = _lba;
	sectors = iovs.bytes() / 512;
	callback = _callback;
	ptr = _ptr;
    }
};

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

size_t getsize64(int fd)
{
    struct stat sb;
    size_t size;
    
    if (fstat(fd, &sb) < 0)
	throw_fs_error("stat");
    if (S_ISBLK(sb.st_mode)) {
	if (ioctl(fd, BLKGETSIZE64, &size) < 0)
	    throw_fs_error("ioctl");
    }
    else
	size = sb.st_size;
    return size;
}

/* libaio helpers */

void e_iocb_cb(io_context_t ctx, iocb *io, long res, long res2);

struct e_iocb {
    iocb io;
    void (*cb)(void*) = NULL;
    void *ptr = NULL;
    e_iocb() { io_set_callback(&io, e_iocb_cb); }
};

void e_iocb_cb(io_context_t ctx, iocb *io, long res, long res2)
{
    auto iocb = (e_iocb*)io;
    iocb->cb(iocb->ptr);
    delete iocb;
}

int io_queue_wait(io_context_t ctx, struct timespec *timeout)
{
    return io_getevents(ctx, 0, 0, NULL, timeout);
}

void e_iocb_runner(io_context_t ctx, bool *running, const char *name)
{
    int rv;
    pthread_setname_np(pthread_self(), name);
    while (*running) {
	if ((rv = io_queue_run(ctx)) < 0)
	    break;
	if (rv == 0)
	    usleep(100);
	if (io_queue_wait(ctx, NULL) < 0)
	    break;
    }
}

void e_io_prep_pwrite(e_iocb *io, int fd, void *buf, size_t len, size_t offset,
		      void (*cb)(void*), void *arg)
{
    io_prep_pwrite(&io->io, fd, buf, len, offset);
    io->cb = cb;
    io->ptr = arg;
    io_set_callback(&io->io, e_iocb_cb);
}

void e_io_prep_pread(e_iocb *io, int fd, void *buf, size_t len, size_t offset,
		     void (*cb)(void*), void *arg)
{
    io_prep_pread(&io->io, fd, buf, len, offset);
    io->cb = cb;
    io->ptr = arg;
    io_set_callback(&io->io, e_iocb_cb);
}

void e_io_prep_pwritev(e_iocb *io, int fd, const struct iovec *iov, int iovcnt,
		     size_t offset, void (*cb)(void*), void *arg)
{
    io_prep_pwritev(&io->io, fd, iov, iovcnt, offset);
    io->cb = cb;
    io->ptr = arg;
    io_set_callback(&io->io, e_iocb_cb);
}

void e_io_prep_preadv(e_iocb *eio, int fd, const struct iovec *iov, int iovcnt,
		    size_t offset, void (*cb)(void*), void *arg)
{
    io_prep_preadv(&eio->io, fd, iov, iovcnt, offset);
    eio->cb = cb;
    eio->ptr = arg;
    io_set_callback(&eio->io, e_iocb_cb);
}

int e_io_submit(io_context_t ctx, e_iocb *eio)
{
    iocb *io = &eio->io;
    return io_submit(ctx, 1, &io);
}

/* misc helpers stuff */

static bool aligned(const void *ptr, int a)
{
    return 0 == ((long)ptr & (a-1));
}

/* convenience class, because we don't know cache size etc.
 * at cache object construction time.
 */
template <class T>
class sized_vector {
    std::vector<T> *elements;
public:
    ~sized_vector() {
	delete elements;
    }
    void init(int n) {
	elements = new std::vector<T>(n);
    }
    void init(int n, T val) {
	elements = new std::vector<T>(n, val);
    }
    T &operator[](int index) {
	return (*elements)[index];
    }
};

/* the read cache is:
 * 1. indexed by obj/offset[*], not LBA
 * 2. stores aligned 64KB blocks 
 * [*] offset is in units of 64KB blocks
 */
class read_cache {
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
    void evict(int n) {
	printf("\nEVICTING %d\n", n);
	// assert(!m.try_lock());	// m must be locked
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

    void evict_thread(thread_pool<int> *p) {
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

	    if (!map_dirty)	// free list didn't change
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

public:
    std::atomic<int> n_lines_read = 0;

    read_cache(uint32_t blkno, int _fd, bool nt, translate *_be, objmap *_om, backend *_io) :
	omap(_om), be(_be), fd(_fd), io(_io), misc_threads(&m), nothreads(nt)
    {
	dev_max = getsize64(fd);
	n_lines_read = 0;
	char *buf = (char*)aligned_alloc(512, 4096);
	if (pread(fd, buf, 4096, 4096L*blkno) < 4096)
	    throw_fs_error("rcache3");
	super = (j_read_super*)buf;
	
	assert(super->unit_size == 128); // 64KB, in sectors
	unit_sectors = super->unit_size; // todo: fixme
	
	int oos_per_pg = 4096 / sizeof(extmap::obj_offset);
	assert(div_round_up(super->units, oos_per_pg) == super->map_blocks);

	flat_map = (extmap::obj_offset*)aligned_alloc(512, super->map_blocks*4096);
	if (pread(fd, (char*)flat_map, super->map_blocks*4096, super->map_start*4096L) < 0)
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

	misc_threads.pool.push(std::thread(&read_cache::evict_thread, this, &misc_threads));

	io_queue_init(64, &ioctx);
	e_io_running = true;
	const char *name = "read_cache_cb";
	e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
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
    char *get_cacheline_buf(int n) {
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

    /* returns skip_len, read_len. Fetches from backend directly, so skip_len >0
     * means unmapped sectors that should be zeroed.
     */
    int u1 = 0;
    int u0 = 0;
    std::pair<size_t,size_t> async_read(size_t offset, char *buf, size_t len,
					void (*cb)(void*), void *ptr) {
	lba_t base = offset/512, sectors = len/512, limit = base+sectors;
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
	int n = -1;		// cache block number

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
	    else {		// prior read is pending
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
	    u1++;
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

	    auto write_done = wrap([this, n]{
		    written[n] = true;
		    return true;
		});

	    auto read_done = wrap([this, n, write_done, nvme_offset, buf_offset, bytes,
				   _buf, buf, cb, ptr]{
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

	    io->aio_read_num_object(unit.obj, _buf, 512L*unit_sectors,
				    512L*blk_base, call_wrapped, read_done);
	    read_len = bytes;
	}
	else {
	    u0++;
	    hit_stats.user += read_len / 512;
	    hit_stats.backend += read_len / 512;
	    lk2.unlock();
	    io->aio_read_num_object(oo.obj, buf, read_len, 512L*oo.offset, cb, ptr);
	    // read_len unchanged
	}
	return std::make_pair(skip_len, read_len);
    }

    void write_map(void) {
	pwrite(fd, flat_map, 4096 * super->map_blocks, 4096L * super->map_start);
    }
    
    ~read_cache() {
#if 1
	printf("rc: map %ld (%ld)\n", map.size(), sizeof(std::pair<extmap::obj_offset,int>));
	printf("rc: usecache 1 %d 0 %d (stat.u %ld .b %ld)\n", u1, u0, hit_stats.user, hit_stats.backend);
#endif
	free((void*)flat_map);
	free((void*)super);

	e_io_running = false;
	e_io_th.join();
	io_queue_release(ioctx);
    }

    /* debugging. 
     */

    void get_info(j_read_super **p_super, extmap::obj_offset **p_flat, 
		  std::vector<int> **p_free_blks, std::map<extmap::obj_offset,int> **p_map) {
	if (p_super != NULL)
	    *p_super = super;
	if (p_flat != NULL)
	    *p_flat = flat_map;
	if (p_free_blks != NULL)
	    *p_free_blks = &free_blks;
	if (p_map != NULL)
	    *p_map = &map;
    }

    /* add
     */
    void do_add(extmap::obj_offset unit, char *buf) {
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
    std::mutex m;
    std::map<int,int> cached_fds;
    std::queue<int>   cached_nums;
    static const int  fd_cache_size = 500;

    int get_cached_fd(int seq) {
	std::unique_lock lk(m);
	auto it = cached_fds.find(seq);
	if (it != cached_fds.end())
	    return cached_fds[seq];

	if (cached_nums.size() >= fd_cache_size) {
	    auto num = cached_nums.front();
	    close(cached_fds[num]);
	    cached_fds.erase(num);
	    cached_nums.pop();
	}
	auto name = std::string(prefix) + "." + hex(seq);
	auto fd = open(name.c_str(), O_RDONLY);
	if (fd < 0)
	    throw_fs_error("read_obj_open");
	cached_fds[seq] = fd;
	cached_nums.push(seq);
	return fd;
    }

    bool e_io_running = false;
    io_context_t ioctx;
    std::thread e_io_th;

public:
    file_backend(const char *_prefix) {
	prefix = strdup(_prefix);
	e_io_running = true;
	io_queue_init(64, &ioctx);
	const char *name = "file_backend_cb";
	e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
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
    void delete_numbered_object(int seq) {
	auto name = std::string(prefix) + "." + hex(seq);
	unlink(name.c_str());
    }
    ssize_t read_object(const char *name, char *buf, size_t len, size_t offset) {
	int fd = open(name, O_RDONLY);
	if (fd < 0)
	    return -1;
	auto val = pread(fd, buf, len, offset);
	close(fd);
	if (val < 0)
	    throw_fs_error("read_obj");
	return val;
    }
    ssize_t read_numbered_object(int seq, char *buf, size_t len, size_t offset) {
	iovec iov = {buf, len};
	return read_numbered_objectv(seq, &iov, 1, offset);
    }
    
    ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset) {
	auto fd = get_cached_fd(seq);
	auto val = preadv(fd, iov, iovcnt, offset);
	if (val < 0)
	    throw_fs_error("read_obj");
	return val;
    }
    
    int aio_read_num_object(int seq, char *buf, size_t len,
			    size_t offset, void (*cb)(void*), void *ptr) {
	int fd = get_cached_fd(seq);
	auto eio = new e_iocb;
	e_io_prep_pread(eio, fd, buf, len, offset, cb, ptr);
	e_io_submit(ioctx, eio);
	return 0;
    }
    
    int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
				  void (*cb)(void*), void *ptr) {
	auto name = std::string(prefix) + "." + hex(seq);
	int fd = open(name.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0777);
	if (fd < 0)
	    return -1;

	auto closure = wrap([fd, cb, ptr]{
		close(fd);
		cb(ptr);
		return true;
	    });
	auto eio = new e_iocb;
	size_t offset = 0;
	e_io_prep_pwritev(eio, fd, iov, iovcnt, offset, call_wrapped, closure);
	e_io_submit(ioctx, eio);
	return 0;
    }
    
    ~file_backend() {
	free((void*)prefix);
	for (auto it = cached_fds.begin(); it != cached_fds.end(); it++)
	    close(it->second);

	e_io_running = false;
	e_io_th.join();
	io_queue_release(ioctx);
    }
    std::string object_name(int seq) {
	return std::string(prefix) + "." + hex(seq);
    }
};

/* all addresses are in units of 4KB blocks
 */
class write_cache {
    int            fd;
    size_t         dev_max;
    uint32_t       super_blkno;
    j_write_super *super;	// 4KB

    std::atomic<int64_t> sequence; // write sequence #
    
    extmap::cachemap2 map;
    extmap::cachemap2 rmap;
    std::map<page_t,int> lengths;
    translate        *be;
    bool              map_dirty;

    std::vector<cache_work*> work;
    int                      writes_outstanding = 0;

    page_t evict_trigger;
    
    thread_pool<int>          *misc_threads;
    std::mutex                m;
    std::condition_variable   alloc_cv;
    int                       nfree;
    
    char *pad_page;

    bool e_io_running = false;
    io_context_t ioctx;
    std::thread e_io_th;
    
    int pages_free(uint32_t oldest) {
	auto size = super->limit - super->base;
	auto tail = (super->next >= oldest) ? oldest + size : oldest;
	return tail - super->next - 1;
    }
    
    uint32_t allocate_locked(page_t n, page_t &pad,
			     std::unique_lock<std::mutex> &lk) {
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
    uint32_t allocate(page_t n, page_t &pad) {
	std::unique_lock lk(m);
	return allocate_locked(n, pad, lk);
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

    void evict(void) {
	assert(!m.try_lock());	// m must be locked
	auto oldest = super->oldest;
	int pgs_free = pages_free(oldest);
	assert(pgs_free <= evict_trigger);

	std::vector<j_extent> to_delete;
	while (pages_free(oldest) < evict_trigger*3) {
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
    }

    void evict_thread(thread_pool<int> *p) {
	auto period = std::chrono::milliseconds(10);
	const int evict_min_pct = 5;
	const int evict_max_mb = 100;
	int trigger;
	{			// make valgrind happy
	    std::unique_lock lk(m);
	    trigger = std::min((evict_min_pct * (int)(super->limit - super->base) / 100),
			       evict_max_mb * (1024*1024/4096));
	}
	auto t0 = std::chrono::system_clock::now();
	auto super_timeout = std::chrono::milliseconds(500);
	
	while (p->running) {
	    std::unique_lock lk(m);
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
		    //xprintf("lens[%d] : %d\n", oldest, len);
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
		write_checkpoint();
		
		auto t = std::chrono::system_clock::now();
		if (t - t0 >= super_timeout) {
		    if (pwrite(fd, super, 4096, 4096L*super_blkno) < 0)
			throw_fs_error("wsuper_rewrite");
		    t0 = t;
		}
	    }
	}
    }

    void ckpt_thread(thread_pool<int> *p) {
	auto next0 = super->next, N = super->limit - super->base;
	auto period = std::chrono::milliseconds(100);
	auto t0 = std::chrono::system_clock::now();
	auto timeout = std::chrono::seconds(5);
	const int ckpt_interval = N / 4;

	while (p->running) {
	    std::unique_lock lk(m);
	    p->cv.wait_for(lk, period);
	    if (!p->running)
		return;
	    auto t = std::chrono::system_clock::now();
	    bool do_ckpt = (int)((super->next + N - next0) % N) > ckpt_interval ||
		((t - t0 > timeout) && map_dirty);
	    if (p->running && do_ckpt) {
		next0 = super->next;
		t0 = t;
		lk.unlock();
		write_checkpoint();
	    }
	}
    }

    bool ckpt_in_progress = false;

    void write_checkpoint(void) {
	std::unique_lock<std::mutex> lk(m);
	if (ckpt_in_progress)
	    return;
	ckpt_in_progress = true;

	size_t map_bytes = map.size() * sizeof(j_map_extent);
	size_t len_bytes = lengths.size() * sizeof(j_length);
	page_t map_pages = div_round_up(map_bytes, 4096),
	    len_pages = div_round_up(len_bytes, 4096),
	    ckpt_pages = map_pages + len_pages;

	/* TODO - switch between top/bottom of metadata region so we
	 * don't leave in inconsistent state if we crash during checkpoint
	 */
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
	for (auto it = lengths.begin(); it != lengths.end(); it++)
	    jl[n_lens++] = (j_length){it->first, it->second};

	// valgrind:
	int pad2 = 4096L * len_pages - len_bytes;
	assert(len_bytes + pad2 == len_pages*4096UL);
	if (pad2 > 0)
	    memset(l_buf + len_bytes, 0, pad2);

	j_write_super *super_copy = (j_write_super*)aligned_alloc(512, 4096);
	memcpy(super_copy, super, 4096);

	super_copy->map_start = super->map_start = blockno;
	super_copy->map_blocks = super->map_blocks = map_pages;
	super_copy->map_entries = super->map_entries = n_extents;

	super_copy->len_start = super->len_start = blockno+map_pages;
	super_copy->len_blocks = super->len_blocks = len_pages;
	super_copy->len_entries = super->len_entries = n_lens;

	lk.unlock();

	assert(4096UL*blockno + 4096UL*ckpt_pages <= dev_max);
	iovec iov[] = {{e_buf, map_pages*4096UL},
		       {l_buf, len_pages*4096UL}};

	if (pwritev(fd, iov, 2, 4096L*blockno) < 0)
	    throw_fs_error("wckpt_e");
	if (pwrite(fd, (char*)super_copy, 4096, 4096L*super_blkno) < 0)
	    throw_fs_error("wckpt_s");

	free(super_copy);
	free(e_buf);
	free(l_buf);

	map_dirty = false;
	ckpt_in_progress = false;
    }

public:
    write_cache(uint32_t blkno, int _fd, translate *_be, int n_threads) {
	super_blkno = blkno;
	fd = _fd;
	dev_max = getsize64(fd);
	be = _be;
	char *buf = (char*)aligned_alloc(512, 4096);
	if (pread(fd, buf, 4096, 4096L*blkno) < 4096)
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
	    if (pread(fd, map_buf, map_bytes_rounded, 4096L * super->map_start) < 0)
		throw_fs_error("wcache_map");
	    decode_offset_len<j_map_extent>(map_buf, 0, map_bytes, extents);
	    for (auto e : extents) {
		map.update(e.lba, e.lba+e.len, e.plba);
		rmap.update(e.plba, e.plba + e.len, e.lba);
	    }
	    free(map_buf);

	    size_t len_bytes = super->len_entries * sizeof(j_length),
		len_bytes_rounded = round_up(len_bytes, 4096);
	    char *len_buf = (char*)aligned_alloc(512, len_bytes_rounded);
	    std::vector<j_length> _lengths;
	    if (pread(fd, len_buf, len_bytes_rounded, 4096L * super->len_start) < 0)
		throw_fs_error("wcache_len");
	    decode_offset_len<j_length>(len_buf, 0, len_bytes, _lengths);
	    for (auto l : _lengths) {
		lengths[l.page] = l.len;
		assert(lengths[l.page] > 0);
		//xprintf("init: lens[%d] = %d\n", l.page, l.len);
	    }
	    free(len_buf);
	}

	/* TODO TODO TODO - need to roll log forward
	 */
	auto N = super->limit - super->base;
	if (super->oldest == super->next)
	    nfree = N - 1;
	else
	    nfree = (super->oldest + N - super->next) % N;

	int evict_min_pct = 5;
	int evict_max_mb = 100;
	evict_trigger = std::min((evict_min_pct * (int)(super->limit - super->base) / 100),
				 evict_max_mb * (1024*1024/4096));
	
	// https://stackoverflow.com/questions/22657770/using-c-11-multithreading-on-non-static-member-function

	misc_threads = new thread_pool<int>(&m);
	misc_threads->pool.push(std::thread(&write_cache::ckpt_thread, this, misc_threads));

	e_io_running = true;
	io_queue_init(64, &ioctx);
	const char *name = "write_cache_cb";
	e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
    }
    ~write_cache() {
#if 0
	printf("wc map: %d %d (%ld)\n", map.size(), map.capacity(), sizeof(extmap::lba2lba));
	printf("wc rmap: %d %d (%ld)\n", rmap.size(), rmap.capacity(), sizeof(extmap::lba2lba));
#endif
	free(pad_page);
	free(super);
	delete misc_threads;

	e_io_running = false;
	e_io_th.join();
	io_queue_release(ioctx);
    }

    void send_writes(void) {
	std::unique_lock lk(m);
	writes_outstanding++;
	auto w = new std::vector<cache_work*>(std::make_move_iterator(work.begin()),
					      std::make_move_iterator(work.end()));
	work.erase(work.begin(), work.end());

	sector_t sectors = 0;
	for (auto _w : *w) {
	    sectors += _w->sectors;
	    assert(_w->iovs.aligned(512));
	}

	page_t blocks = div_round_up(sectors, 8);
	char *pad_hdr = NULL;
	page_t pad, blockno = allocate_locked(blocks+1, pad, lk);

	if (pad != 0) 
	    lengths[pad] = super->limit - pad;
	lengths[blockno] = blocks+1;
	lk.unlock();
	
	if (pad != 0) {
	    assert((pad+1)*4096UL <= dev_max);
	    pad_hdr = (char*)aligned_alloc(512, 4096);
	    auto closure = wrap([pad_hdr]{
		    free(pad_hdr);
		    return true;
		});
	    mk_header(pad_hdr, LSVD_J_PAD, my_uuid, (super->limit - pad));
	    auto eio = new e_iocb;
	    e_io_prep_pwrite(eio, fd, pad_hdr, 4096, pad*4096L, call_wrapped, closure);
	    e_io_submit(ioctx, eio);
	}

	std::vector<j_extent> extents;
	for (auto _w : *w)
	    extents.push_back((j_extent){_w->lba, (uint64_t)_w->sectors});
		
        char *hdr = (char*)aligned_alloc(512, 4096);
	j_hdr *j = mk_header(hdr, LSVD_J_DATA, my_uuid, 1+blocks);

	j->extent_offset = sizeof(*j);
	size_t e_bytes = extents.size() * sizeof(j_extent);
	j->extent_len = e_bytes;
	memcpy((void*)(hdr + sizeof(*j)), (void*)extents.data(), e_bytes);
	
	lba_t plba = (blockno+1) * 8;
	auto iovs = new smartiov();
	iovs->push_back((iovec){hdr, 4096});
	for (auto _w : *w) {
	    auto [iov, iovcnt] = _w->iovs.c_iov();
	    iovs->ingest(iov, iovcnt);
	}
	
	auto closure = wrap([this, hdr, plba, iovs, w] {
		/* first update the maps */
		std::vector<extmap::lba2lba> garbage; 
		std::unique_lock lk(m);
		auto _plba = plba;
		for (auto _w : *w) {
		    map.update(_w->lba, _w->lba + _w->sectors, _plba, &garbage);
		    rmap.update(plba, _plba+_w->sectors, _w->lba);
		    _plba += _w->sectors;
		    map_dirty = true;
		}
		for (auto it = garbage.begin(); it != garbage.end(); it++) 
		    rmap.trim(it->s.base, it->s.base+it->s.len);

		/* then call back, send to backend */
		lk.unlock();
		for (auto _w : *w) {
		    be->writev(_w->lba*512, _w->iovs.data(), _w->iovs.size());
		    _w->callback(_w->ptr);
		    delete _w;
		}

		/* and finally clean everything up */
		free(hdr);
		delete iovs;
		delete w;

		lk.lock();
		--writes_outstanding;
		if (work.size() > 0) {
		    lk.unlock();
		    send_writes();
		}
		return true;
	    });

	auto eio = new e_iocb;
	assert(blockno+iovs->bytes()/4096L <= super->limit);
	e_io_prep_pwritev(eio, fd, iovs->data(), iovs->size(), blockno*4096L,
			  call_wrapped, closure);
	e_io_submit(ioctx, eio);
    }

    bool evicting = false;
    
    void writev(size_t offset, const iovec *iov, int iovcnt, void (*cb)(void*), void *ptr) {
	auto w = new cache_work(offset/512L, iov, iovcnt, cb, ptr);
	std::unique_lock lk(m);

	while (pages_free(super->oldest) <= evict_trigger) {
	    if (evicting)
		alloc_cv.wait(lk);
	    else {
		evicting = true;
		evict();
		evicting = false;
	    }
	}
	work.push_back(w);
	if (writes_outstanding < 4 || work.size() >= 8) {
	    lk.unlock();
	    send_writes();
	}
    }
    
    void writev2(size_t offset, const iovec *iov, int iovcnt, void (*cb)(void*), void *ptr) {
	size_t len = iov_sum(iov, iovcnt);
	sector_t sectors = len / 512, lba = offset / 512;
	page_t blocks = div_round_up(sectors, 8);
	char *pad_hdr = NULL;
	
	// allocate blocks + 1
	std::unique_lock lk(m);
	page_t pad, blockno = allocate_locked(blocks+1, pad, lk);

	if (pad != 0) 
	    lengths[pad] = super->limit - pad;
	lengths[blockno] = blocks+1;
	lk.unlock();

	if (pad != 0) {
	    assert((pad+1)*4096UL <= dev_max);
	    pad_hdr = (char*)aligned_alloc(512, 4096);
	    auto closure = wrap([pad_hdr]{
		    free(pad_hdr);
		    return true;
		});
	    mk_header(pad_hdr, LSVD_J_PAD, my_uuid, (super->limit - pad));
	    auto eio = new e_iocb;
	    e_io_prep_pwrite(eio, fd, pad_hdr, 4096, pad*4096L, call_wrapped, closure);
	    e_io_submit(ioctx, eio);
	}

	for (int i = 0; i < iovcnt; i++)
	    assert(aligned(iov[i].iov_base, 512));

        char *hdr = (char*)aligned_alloc(512, 4096);
	j_hdr *j = mk_header(hdr, LSVD_J_DATA, my_uuid, 1+blocks);
	j_extent ext = {(uint64_t)lba, (uint64_t)sectors};

	j->extent_offset = sizeof(*j);
	size_t e_bytes = sizeof(j_extent);
	j->extent_len = e_bytes;
	memcpy((void*)(hdr + sizeof(*j)), &ext, e_bytes);
	
	lba_t plba = (blockno+1) * 8;
	iovec hdr_iov = (iovec){.iov_base = hdr, .iov_len = 4096};
	auto s_iovs = new smartiov(&hdr_iov, 1);
	s_iovs->ingest(iov, iovcnt);
	
	auto closure = wrap(
	    [this, hdr, iov, iovcnt, cb, ptr, lba, sectors, plba, j, s_iovs]
	    {
		/* first update the maps */
		std::vector<extmap::lba2lba> garbage; 
		std::unique_lock lk(m);
		map.update(lba, lba + sectors, plba, &garbage);
		rmap.update(plba, plba+sectors, lba);
		for (auto it = garbage.begin(); it != garbage.end(); it++) 
		    rmap.trim(it->s.base, it->s.base+it->s.len);
		map_dirty = true;
		lk.unlock();

		/* then call back, send to backend */
		be->writev(lba*512, (iovec*)iov, iovcnt);
		cb(ptr);

		/* and finally clean everything up */
		free(hdr);
		delete s_iovs;
		return true;
	    });

	auto eio = new e_iocb;
	e_io_prep_pwritev(eio, fd, s_iovs->data(), s_iovs->size(), blockno*4096L,
			  call_wrapped, closure);
	e_io_submit(ioctx, eio);
    }
    
    /* returns (number of bytes skipped), (number of bytes read_started)
     */
    std::pair<size_t,size_t> async_read(size_t offset, char *buf, size_t bytes,
					void (*cb)(void*), void *ptr) {
	lba_t base = offset/512, limit = base + bytes/512;
	std::unique_lock<std::mutex> lk(m);
	off_t nvme_offset = 0;
	size_t skip_len = 0, read_len = 0;
	
	auto it = map.lookup(base);
	if (it == map.end() || it->base() >= limit)
	    skip_len = bytes;
	else {
	    auto [_base, _limit, plba] = it->vals(base, limit);
	    if (_base > base) {
		skip_len = 512 * (_base - base);
		buf += skip_len;
	    }
	    read_len = 512 * (_limit - _base),
		nvme_offset = 512L * plba;
	}
	lk.unlock();
	if (read_len) {
	    auto eio = new e_iocb;
	    e_io_prep_pread(eio, fd, buf, read_len, nvme_offset, cb, ptr);
	    e_io_submit(ioctx, eio);
	}
	return std::make_pair(skip_len, read_len);
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
    /* debug:
     * cache eviction - get info from oldest entry in cache. [should be private]
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
    
    void do_write_checkpoint(void) {
	if (map_dirty)
	    write_checkpoint();
    }
};

/* -------------- RADOS ------------ */

static std::pair<std::string,std::string> split_string(std::string s, std::string delim)
{
    auto i = s.find(delim);
    return std::pair(s.substr(0,i), s.substr(i+delim.length()));
}

#include <rados/librados.h>

class rados_backend : public backend {
    std::mutex m;
    char *pool;
    char *prefix;
    rados_t cluster;
    rados_ioctx_t io_ctx;

public:
    rados_backend(const char *_prefix) {
	int r;
	auto [_pool, _key] = split_string(std::string(_prefix), "/");
	if ((r = rados_create(&cluster, NULL)) < 0) // NULL = ".client"
	    throw("rados create");
	if ((r = rados_conf_read_file(cluster, NULL)) < 0)
	    throw("rados conf");
	if ((r = rados_connect(cluster)) < 0)
	    throw("rados connect");
        if ((r = rados_ioctx_create(cluster, _pool.c_str(), &io_ctx)) < 0)
	    throw("rados ioctx_create");
	prefix = strdup(_key.c_str());
    }
    ssize_t write_object(const char *name, iovec *iov, int iovcnt) {
	smartiov iovs(iov, iovcnt);
	char *buf = (char*)malloc(iovs.bytes());
	iovs.copy_out(buf);
	int r = rados_write(io_ctx, name, buf, iovs.bytes(), 0);
	free(buf);
	return r;
    }
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) {
	char name[128];
	sprintf(name, "%s.%08x", prefix, seq);
	//auto name = std::string(prefix) + "." + hex(seq);
	return write_object(name, iov, iovcnt);
    }
    void delete_numbered_object(int seq) {
	char name[128];
	sprintf(name, "%s.%08x", prefix, seq);
	rados_remove(io_ctx, name);
    }
    ssize_t read_object(const char *name, char *buf, size_t len, size_t offset) {
	return rados_read(io_ctx, name, buf, len, offset);
    }
    ssize_t read_numbered_object(int seq, char *buf, size_t len, size_t offset) {
	//auto name = std::string(prefix) + "." + hex(seq);
	char name[128];
	sprintf(name, "%s.%08x", prefix, seq);
	//printf("p: %s +: %s\n", name2, name.c_str());
	return read_object(name, buf, len, offset);
    }
    
    ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset) {
	smartiov iovs(iov, iovcnt);
	char *buf = (char*)malloc(iovs.bytes());
	int r = read_numbered_object(seq, buf, iovs.bytes(), offset);
	iovs.copy_in(buf);
	free(buf);
	return r;
    }
    struct rados_aio {
	void (*cb)(void*);
	void *ptr;
	rados_completion_t c;
    };
    static void aio_read_done(rados_completion_t c, void *ptr) {
	auto aio = (rados_aio*)ptr;
	aio->cb(aio->ptr);
	rados_aio_release(aio->c);
	delete aio;
    }
    int aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
			void (*cb)(void*), void *ptr)
    {
	auto name = std::string(prefix) + "." + hex(seq);
	rados_aio *aio = new rados_aio;
	aio->cb = cb;
	aio->ptr = ptr;
	assert(buf != NULL);
	rados_aio_create_completion((void*)aio, aio_read_done, NULL, &aio->c);
	return rados_aio_read(io_ctx, name.c_str(), aio->c, buf, len, offset);
    }
    int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
				  void (*cb)(void*), void *ptr) {
	auto name = std::string(prefix) + "." + hex(seq);
	auto rv = write_object(name.c_str(), iov, iovcnt);
	cb(ptr);
	return rv;
    }
    ~rados_backend() {
	free((void*)prefix);
	rados_ioctx_destroy(io_ctx);
	rados_shutdown(cluster);
    }
    std::string object_name(int seq) {
	return std::string(prefix) + "." + hex(seq);
    }
};

/* ------------------- DEBUGGING ----------------------*/

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

// typedef void *rados_ioctx_t;
// typedef void *rados_t;
// typedef void *rados_config_t;

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
    bool done = false;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<int> refcount = 0;
    std::atomic<int> n = 0;
    iovec iov;			// occasional use only
    
    lsvd_completion() {}
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

extern "C" int rbd_aio_read(rbd_image_t image, uint64_t offset, size_t len, char *buf,
			    rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    char *aligned_buf = buf;
    //assert(aligned(buf, 512));
    if (!aligned(buf, 512))
	aligned_buf = (char*)aligned_alloc(512, len);
    auto p = (lsvd_completion*)c;
    p->fri = fri;

    assert(p != NULL);

    /* god, I've got to straighten out all the reference counting stuff.
     * put a reference on, so that we can get through the loops without 
     * completing prematurely
     */
    p->n.store(1);
    char *_buf = aligned_buf;	// read and increment these ones
    size_t _len = len;
    
    while (_len > 0) {
	/* this is ugly. Need to put the closure here, to capture the proper values
	 * of 'buf' and 'len'.
	 */
	auto closure = wrap([p, aligned_buf, buf, len]{
		if (0 == --p->n) {
		    if (aligned_buf != buf) 
			memcpy(buf, aligned_buf, len);
		    p->get();
		    p->complete(0);
		    p->put();
		    if (aligned_buf != buf) 
			free(aligned_buf);
		    return true;
		}
		return false;
	    });

	bool closure_used = false;
	p->n++;
	auto [skip,wait] =
	    fri->wcache->async_read(offset, _buf, _len, call_wrapped, closure);
	if (wait == 0)
	    p->n--;
	else
	    closure_used = true;
	_len -= skip;
	while (skip > 0) {
	    p->n++;
	    auto [skip2, wait2] =
		fri->rcache->async_read(offset, _buf, skip, call_wrapped, closure);
	    if (wait2 == 0)
		p->n--;
	    else
		closure_used = true;
	    memset(_buf, 0, skip2);
	    skip -= (skip2 + wait2);
	    _buf += (skip2 + wait2);
	    offset += (skip2 + wait2);
	}
	_buf += wait;
	_len -= wait;
	offset += wait;
	if (!closure_used)
	    delete_wrapped(closure);
    }

    /* ugly - now I have to repeast the closure code to remove the reference
     * from up top LOOKUP
     */
    if (0 == --p->n) {
	if (aligned_buf != buf) 
	    memcpy(buf, aligned_buf, len);
	p->get();
	p->complete(0);
	p->put();
	if (aligned_buf != buf) 
	    free(aligned_buf);
    }
    
    return 0;
}

/* TODO - add optional buffer to lsvd_completion, 
 *   completion copies (for read) and frees 
 */
extern "C" int rbd_aio_readv(rbd_image_t image, const iovec *iov,
			     int iovcnt, uint64_t off, rbd_completion_t c)
{
    return 0;
}

extern "C" int rbd_aio_writev(rbd_image_t image, const struct iovec *iov,
			      int iovcnt, uint64_t off, rbd_completion_t c)
{
    return 0;
}

extern "C" int rbd_aio_write(rbd_image_t image, uint64_t off, size_t len, const char *buf,
			     rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = fri;

    char *aligned_buf = (char*)buf;
    if (!aligned(buf, 512)) {
	aligned_buf = (char*)aligned_alloc(512, len);
	memcpy(aligned_buf, buf, len);
    }
    
    auto closure = wrap([p, buf, aligned_buf]{
	    p->get();
	    p->complete(0);
	    p->put();
	    if (aligned_buf != buf)
		free(aligned_buf);
	    return true;
	});
    p->iov = (iovec){aligned_buf, len};
    fri->wcache->writev(off, &p->iov, 1, call_wrapped, closure);

    return 0;
}

void rbd_call_wrapped(rbd_completion_t c, void *ptr)
{
    call_wrapped(ptr);
}

/* note that rbd_aio_read handles aligned bounce buffers for us
 */
extern "C" int rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf)
{
    rbd_completion_t c;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    void *closure = wrap([&m, &cv, &done]{
	    done = true;
	    cv.notify_all();
	    return true;
	});
    rbd_aio_create_completion(closure, rbd_call_wrapped, &c);

    std::unique_lock lk(m);
    rbd_aio_read(image, off, len, buf, c);
    while (!done)
	cv.wait(lk);
    auto val = rbd_aio_get_return_value(c);
    rbd_aio_release(c);
    return val;
}

extern "C" int rbd_write(rbd_image_t image, uint64_t off, size_t len, const char *buf)
{
    rbd_completion_t c;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    void *closure = wrap([&m, &cv, &done]{
	    std::unique_lock lk(m);
	    done = true;
	    cv.notify_all();
	    return true;
	});
    rbd_aio_create_completion(closure, rbd_call_wrapped, &c);

    std::unique_lock lk(m);
    rbd_aio_write(image, off, len, buf, c);
    while (!done)
	cv.wait(lk);
    auto val = rbd_aio_get_return_value(c);
    rbd_aio_release(c);
    return val;
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

fake_rbd_image *the_fri;	// debug
extern "C" int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image,
			const char *snap_name)
{
    int rv;
    auto [nvme, obj] = split_string(std::string(name), ",");
    bool rados = (obj.substr(0,6) == "rados:");
    auto fri = new fake_rbd_image;
    
    if (rados)
	fri->io = new rados_backend(obj.c_str()+6);
    else
	fri->io = new file_backend(obj.c_str());
    fri->omap = new objmap();
    fri->lsvd = new translate(fri->io, fri->omap);
    int n_xlate_threads = 3;
    char *nxt = getenv("N_XLATE");
    if (nxt) {
	n_xlate_threads = atoi(nxt);
    }
    const char *base = obj.c_str();
    if (rados) {
	auto [_tmp, key] = split_string(obj, "/");
	base = key.c_str();
    }
    fri->size = fri->lsvd->init(base, n_xlate_threads, true);
    
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
    }
    
    fri->wcache = new write_cache(js->write_super, fd, fri->lsvd, n_wc_threads);
    fri->rcache = new read_cache(js->read_super, fd, false, fri->lsvd, fri->omap, fri->io);
    fri->notify = false;

    the_fri = fri;
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

/* any following functions are stubs only
 */
extern "C" int rbd_invalidate_cache(rbd_image_t image)
{
    return 0;
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

/* debug functions
 */
extern "C" int dbg_lsvd_write(rbd_image_t image, char *buffer, uint64_t offset, uint32_t size)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    iovec iov = {buffer, size};
    size_t val = fri->lsvd->writev(offset, &iov, 1);
    return val < 0 ? -1 : 0;
}

extern "C" int dbg_lsvd_read(rbd_image_t image, char *buffer, uint64_t offset, uint32_t size)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    iovec iov = {buffer, size};
    size_t val = fri->lsvd->readv(offset, &iov, 1);
    return val < 0 ? -1 : 0;
}

extern "C" int dbg_lsvd_flush(rbd_image_t image)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    fri->lsvd->flush();
    return 0;
}

struct _dbg {
public:
    int type = 0;
    translate   *lsvd;
    write_cache *wcache;
    objmap      *omap;
    read_cache  *rcache;
    backend     *io;
    _dbg(int _t, translate *_l, write_cache *_w, objmap *_o, read_cache *_r, backend *_io) :
	type(_t), lsvd(_l), wcache(_w), omap(_o), rcache(_r), io(_io) {}
};

extern "C" int xlate_open(char *name, int n, bool flushthread, void **p)
{
    auto io = new file_backend(name);
    auto omap = new objmap();
    auto lsvd = new translate(io, omap);
    auto rv = lsvd->init(name, n, flushthread);
    auto d = new _dbg(1, lsvd, NULL, omap, NULL, io);
    *p = (void*)d;
    return rv;
}

extern "C" void xlate_close(_dbg *d)
{
    assert(d->type == 1);
    d->lsvd->shutdown();
    delete d->lsvd;
    delete d->omap;
    delete d->io;
    delete d;
}

extern "C" int xlate_flush(_dbg *d)
{
    assert(d->type == 1);
    return d->lsvd->flush();
}


extern "C" int xlate_size(_dbg *d)
{
    assert(d->type == 1);
    return d->lsvd->mapsize();
}

extern "C" int xlate_read(_dbg *d, char *buffer, uint64_t offset, uint32_t size)
{
    assert(d->type == 1);
    iovec iov = {buffer, size};
    size_t val = d->lsvd->readv(offset, &iov, 1);
    return val < 0 ? -1 : 0;
}

extern "C" int xlate_write(_dbg *d, char *buffer, uint64_t offset, uint32_t size)
{
    assert(d->type == 1);
    iovec iov = {buffer, size};
    size_t val = d->lsvd->writev(offset, &iov, 1);
    return val < 0 ? -1 : 0;
}

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

static int getmap_cb(void *ptr, int base, int limit, int obj, int offset)
{
    getmap_s *s = (getmap_s*)ptr;
    if (s->i < s->max) 
	s->t[s->i++] = (tuple){base, limit, obj, offset, 0};
    return s->i < s->max;
}

extern "C" int xlate_getmap(_dbg *d, int base, int limit, int max, struct tuple *t)
{
    assert(d->type == 1);
    getmap_s s = {0, max, t};
    d->lsvd->getmap(base, limit, getmap_cb, (void*)&s);
    return s.i;
}

extern "C" int xlate_frontier(_dbg *d)
{
    assert(d->type == 1);
    return d->lsvd->frontier();
}

extern "C" void xlate_reset(_dbg *d)
{
    assert(d->type == 1);
    d->lsvd->reset();
}

extern "C" int xlate_checkpoint(_dbg *d)
{
    assert(d->type == 1);
    return d->lsvd->checkpoint();
}

extern "C" void wcache_open(_dbg *d, uint32_t blkno, int fd, void **p)
{
    assert(d->type == 1);
    auto wcache = new write_cache(blkno, fd, d->lsvd, 2);
    *p = (void*)wcache;
}

extern "C" void wcache_close(write_cache *wcache)
{
    delete wcache;
}

extern "C" void wcache_read(write_cache *wcache, char *buf, uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not aligned
    int _len = len;
    std::condition_variable cv;
    std::mutex m;
    for (char *_buf = buf2; _len > 0; ) {
	std::unique_lock lk(m);
	bool done = false;
	void *closure = wrap([&done, &cv, &m]{
		std::unique_lock lk(m);
		done = true;
		cv.notify_all();
		return true;
	    });
	auto [skip_len, read_len] = wcache->async_read(offset, _buf, _len,
						       call_wrapped, closure);
	memset(_buf, 0, skip_len);
	_buf += (skip_len + read_len);
	_len -= (skip_len + read_len);
	offset += (skip_len + read_len);
	if (read_len > 0)
	    while (!done)
		cv.wait(lk);
	else
	    delete_wrapped(closure);
    }
    memcpy(buf, buf2, len);
    free(buf2);
}

extern "C" void wcache_write(write_cache *wcache, char *buf, uint64_t offset, uint64_t len)
{
    char *aligned_buf = (char*)aligned_alloc(512, len);
    memcpy(aligned_buf, buf, len);
    iovec iov = {aligned_buf, len};
    std::condition_variable cv;
    std::mutex m;
    bool done = false;
    void *closure = wrap([&done, &cv, &m]{
	    std::unique_lock lk(m);
	    done = true;
	    cv.notify_all();
	    return true;
	});

    std::unique_lock lk(m);
    wcache->writev(offset, &iov, 1, call_wrapped, closure);

    while (!done)
        cv.wait(lk);
    free(aligned_buf);
}

extern "C" void wcache_img_write(rbd_image_t image, char *buf, uint64_t offset, uint64_t len)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    std::mutex m;
    std::condition_variable cv;
    std::unique_lock lk(m);
    bool done = false;
    void *closure = wrap([&done, &cv, &m]{
	    std::unique_lock lk(m);
	    done = true;
	    cv.notify_all();
	    return true;
	});

    char *aligned_buf = buf;
    if (!aligned(buf, 512)) {
	aligned_buf = (char*)aligned_alloc(512, len);
	memcpy(aligned_buf, buf, len);
    }
    iovec iov = {aligned_buf, len};

    fri->wcache->writev(offset, &iov, 1, call_wrapped, closure);
    while (!done)
	cv.wait(lk);

    if (aligned_buf != buf)
	free(aligned_buf);
}

extern "C" void wcache_reset(write_cache *wcache)
{
    wcache->reset();
}

static int wc_getmap_cb(void *ptr, int base, int limit, int plba)
{
    getmap_s *s = (getmap_s*)ptr;
    if (s->i < s->max)
	s->t[s->i++] = (tuple){base, limit, 0, 0, plba};
    return s->i < s->max;
}

extern "C" int wcache_getmap(write_cache *wcache, int base, int limit, int max, struct tuple *t)
{
    getmap_s s = {0, max, t};
    wcache->getmap(base, limit, wc_getmap_cb, (void*)&s);
    return s.i;
}

extern "C" void wcache_get_super(write_cache *wcache, j_write_super *s)
{
    wcache->get_super(s);
}

extern "C" void wcache_write_ckpt(write_cache *wcache)
{
    wcache->do_write_checkpoint();
}

extern "C" int wcache_oldest(write_cache *wcache, int blk, j_extent *extents, int max, int *p_n)
{
    std::vector<j_extent> exts;
    int next_blk = wcache->get_oldest(blk, exts);
    int n = std::min(max, (int)exts.size());
    memcpy((void*)extents, exts.data(), n*sizeof(j_extent));
    *p_n = n;
    return next_blk;
}

extern "C" void rcache_init(_dbg *d,
			    uint32_t blkno, int fd, void **val_p)
{
    auto rcache = new read_cache(blkno, fd, false,
				 d->lsvd, d->omap, d->io);
    *val_p = (void*)rcache;
}

extern "C" void rcache_shutdown(read_cache *rcache)
{
    delete rcache;
}

extern "C" void rcache_evict(read_cache *rcache, int n)
{
    rcache->do_evict(n);
}

extern "C" void rcache_read(read_cache *rcache, char *buf,
			    uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    int _len = len;
    std::mutex m;
    std::condition_variable cv;
    
    for (char *_buf = buf2; _len > 0; ) {
	bool done = false;
	void *closure = wrap([&m, &cv, &done]{
		done = true;
		cv.notify_all();
		return true;
	    });
	auto [skip_len, read_len] = rcache->async_read(offset, _buf, _len,
						       call_wrapped, closure);
	memset(_buf, 0, skip_len);
	_buf += (skip_len+read_len);
	_len -= (skip_len+read_len);
	offset += (skip_len+read_len);
	if (read_len > 0) {
	    std::unique_lock lk(m);
	    while (!done)
		cv.wait(lk);
	}
	else
	    delete_wrapped(closure);
    }
    memcpy(buf, buf2, len);
    free(buf2);
}

extern "C" void rcache_read2(read_cache *rcache, char *buf,
			    uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    int _len = len;
    std::atomic<int> left(0);
    std::mutex m;
    std::condition_variable cv;
    
    for (char *_buf = buf2; _len > 0; ) {
	void *closure = wrap([&cv, &left]{
		if (--left == 0) {
		    cv.notify_all();
		    return true;
		}
		return false;
	    });
	left++;
	auto [skip_len, read_len] = rcache->async_read(offset, _buf, _len,
						       call_wrapped, closure);
	memset(_buf, 0, skip_len);
	_buf += (skip_len+read_len);
	_len -= (skip_len+read_len);
	offset += (skip_len+read_len);

	if (read_len == 0) {
	    left--;
	    delete_wrapped(closure);
	}
    }
    std::unique_lock lk(m);
    while (left.load() > 0)
	cv.wait(lk);

    memcpy(buf, buf2, len);
    free(buf2);
}

extern "C" void rcache_add(read_cache *rcache, int object, int block, char *buf, size_t len)
{
    assert(len == 65536);
    extmap::obj_offset oo = {object, block};
    rcache->do_add(oo, buf);
}

extern "C" void rcache_getsuper(read_cache *rcache, j_read_super *p_super)
{
    j_read_super *p;
    rcache->get_info(&p, NULL, NULL, NULL);
    *p_super = *p;
}

extern "C" int rcache_getmap(read_cache *rcache,
			     extmap::obj_offset *keys, int *vals, int n)
{
    int i = 0;
    std::map<extmap::obj_offset,int> *p_map;
    rcache->get_info(NULL, NULL, NULL, &p_map);
    for (auto it = p_map->begin(); it != p_map->end() && i < n; it++, i++) {
	auto [key, val] = *it;
	keys[i] = key;
	vals[i] = val;
    }
    return i;
}

extern "C" int rcache_get_flat(read_cache *rcache, extmap::obj_offset *vals, int n)
{
    extmap::obj_offset *p;
    j_read_super *p_super;
    rcache->get_info(&p_super, &p, NULL, NULL);
    n = std::min(n, p_super->units);
    memcpy(vals, p, n*sizeof(extmap::obj_offset));
    return n;
}

extern "C" void rcache_reset(read_cache *rcache)
{
}

extern "C" void fakemap_update(_dbg *d, int base, int limit,
			       int obj, int offset)
{
    extmap::obj_offset oo = {obj,offset};
    d->omap->map.update(base, limit, oo);
}

extern "C" void fakemap_reset(_dbg *d)
{
    d->omap->map.reset();
}

    
