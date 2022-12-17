#include <unistd.h>
#include <uuid/uuid.h>

#include <queue>
#include <map>
#include <thread>

#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>

#include "lsvd_types.h"
#include "smartiov.h"
#include "fake_rbd.h"
#include "config.h"
#include "extent.h"
#include "backend.h"
#include "translate.h"
#include "read_cache.h"
#include "journal.h"
#include "write_cache.h"
#include "image.h"

#include "objects.h"
#include "request.h"

#include "misc_cache.h"
#include "nvme.h"

#include "config.h"

/* types used to interface with some debug functions - must
 * match ctypes definitions in lsvd_types.py
 */
struct _dbg {
public:
    translate   *lsvd;
    write_cache *wcache;
    extmap::objmap    obj_map;
    std::shared_mutex obj_lock;
    read_cache  *rcache;
    backend     *io;
    uuid_t       uuid;
    lsvd_config  cfg;
};

// tuple :	used for retrieving maps
struct tuple {
    int base;
    int limit;
    int obj;                    // object map
    int offset;
    int plba;                   // write cache map
};

// getmap_s :	more helper structures
struct getmap_s {
    int i;
    int max;
    struct tuple *t;
};

extern translate *image_2_xlate(rbd_image_t image);

/* debug functions
 */
extern "C" int dbg_lsvd_write(rbd_image_t image, char *buffer, uint64_t offset, uint32_t size)
{
    auto xlate = image_2_xlate(image);
    iovec iov = {buffer, size};
    size_t val = xlate->writev(0, offset, &iov, 1);
    return val < 0 ? -1 : 0;
}

// struct rbd_image;
extern rbd_image *make_rbd_image(backend *b, translate *t,
				 write_cache *w, read_cache *r);

extern "C" int dbg_lsvd_read(rbd_image_t image, char *buffer, uint64_t offset, uint32_t size)
{
    auto xlate = image_2_xlate(image);
    iovec iov = {buffer, size};
    size_t val = xlate->readv(offset, &iov, 1);
    return val < 0 ? -1 : 0;
}
extern "C" int dbg_lsvd_flush(rbd_image_t image)
{
    auto xlate = image_2_xlate(image);
    xlate->flush();
    return 0;
}
extern "C" int xlate_open(char *name, int n, bool flushthread, void **p)
{
    auto d = new _dbg();
    d->io = make_file_backend(name);
    d->lsvd = make_translate(d->io, &d->cfg, &d->obj_map, &d->obj_lock);
    auto rv = d->lsvd->init(name, n, flushthread);
    *p = (void*)d;
    return rv;
}
extern "C" void xlate_close(_dbg *d)
{
    d->lsvd->shutdown();
    delete d->lsvd;
    delete d->io;
    delete d;
}
extern "C" int xlate_flush(_dbg *d)
{
    return d->lsvd->flush();
}
extern "C" int xlate_size(_dbg *d)
{
    return d->lsvd->mapsize();
}
extern "C" int xlate_read(_dbg *d, char *buffer, uint64_t offset, uint32_t size)
{
    iovec iov = {buffer, size};
    size_t val = d->lsvd->readv(offset, &iov, 1);
    return val < 0 ? -1 : 0;
}
extern "C" int xlate_write(_dbg *d, char *buffer, uint64_t offset, uint32_t size)
{
    iovec iov = {buffer, size};
    size_t val = d->lsvd->writev(0, offset, &iov, 1);
    return val < 0 ? -1 : 0;
}
int getmap_cb(void *ptr, int base, int limit, int obj, int offset)
{
    getmap_s *s = (getmap_s*)ptr;
    if (s->i < s->max) 
	s->t[s->i++] = (tuple){base, limit, obj, offset, 0};
    return s->i < s->max;
}
extern "C" int xlate_getmap(_dbg *d, int base, int limit, int max, struct tuple *t)
{
    getmap_s s = {0, max, t};
    d->lsvd->getmap(base, limit, getmap_cb, (void*)&s);
    return s.i;
}
extern "C" int xlate_frontier(_dbg *d)
{
    return d->lsvd->frontier();
}

extern int batch_seq(translate*);
extern "C" int xlate_seq(_dbg *d)
{
    return batch_seq(d->lsvd);
}

extern "C" void xlate_reset(_dbg *d)
{
    d->lsvd->reset();
}
extern "C" int xlate_checkpoint(_dbg *d)
{
    return d->lsvd->checkpoint();
}
extern "C" void wcache_open(_dbg *d, uint32_t blkno, int fd, void **p)
{
    auto wcache = make_write_cache(blkno, fd, d->lsvd, &d->cfg);
    *p = (void*)wcache;
}
extern "C" void wcache_close(write_cache *wcache)
{
    delete wcache;
}
extern "C" void wcache_read(write_cache *wcache, char *buf, uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not aligned
    std::condition_variable cv;
    std::mutex m;

    smartiov iovs(buf2, len);

    for (size_t _offset = 0; _offset < len; ) {
        std::unique_lock lk(m);
	auto tmp = iovs.slice(_offset, len);
        auto [skip_len, read_len, rreq] = wcache->async_readv(offset, &tmp);

	if (skip_len) 
	    iovs.zero(_offset, _offset + skip_len);

        _offset += (skip_len + read_len);
        offset += (skip_len + read_len);

	if (rreq != NULL) {
	    rreq->run(NULL);
	    rreq->wait();
	    rreq->release();
	}
    }
    memcpy(buf, buf2, len);
    free(buf2);
}

extern "C" void wcache_img_write(rbd_image_t image, char *buf, uint64_t offset, uint64_t len)
{
    rbd_write((rbd_image_t)image, offset, len, buf);
}
extern "C" void wcache_write(write_cache *wcache, char *buf, uint64_t offset, uint64_t len)
{
    auto img = make_rbd_image(NULL, NULL, wcache, NULL);
    wcache_img_write(img, buf, offset, len);
    delete img;
}
int wc_getmap_cb(void *ptr, int base, int limit, int plba)
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
    auto rcache = make_read_cache(blkno, fd, false,
				  d->lsvd, &d->obj_map, &d->obj_lock, d->io);
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

char *logbuf, *p_log, *end_log;
#include <stdarg.h>
std::mutex m;
void do_log(const char *fmt, ...) {
    std::unique_lock lk(m);
    if (!logbuf) {
	size_t sz = 64*1024;
	const char *env = getenv("LSVD_DEBUG_BUF");
	if (env)
	    sz = atoi(env);
	p_log = logbuf = (char*)malloc(sz);
	end_log = p_log + sz;
    }
    va_list args;
    va_start(args, fmt);
    ssize_t max = end_log - p_log - 1;
    if (max > 256)
	p_log += vsnprintf(p_log, max, fmt, args);
}

FILE *_fp;
void fp_log(const char *fmt, ...) {
    //std::unique_lock lk(m);
    if (_fp == NULL)
	_fp = fopen("/tmp/lsvd.log", "w");
    va_list args;
    va_start(args, fmt);
    vfprintf(_fp, fmt, args);
    fflush(_fp);
}

class read1_req : public trivial_request {
    std::condition_variable *cv;
    bool *done;
    
public:
    read1_req(std::condition_variable *cv_, bool *done_) {
	cv = cv_;
	done = done_;
    }
    ~read1_req() { }

    void notify(request *child) {
	*done = true;
	cv->notify_all();
    }
};

/* note that this leaks read cache request structures, because it 
 * doesn't call req->release()
 */
extern "C" void rcache_read(read_cache *rcache, char *buf,
			    uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    std::mutex m;
    std::condition_variable cv;

    smartiov iovs(buf2, len);
    
    for (size_t _offset = 0; _offset < len; ) {
	bool done = false;
	auto tmp = iovs.slice(_offset, len);
	auto [skip_len, read_len, r_req] =
	    rcache->async_readv(offset, &tmp);

	if (skip_len) 
	    iovs.zero(_offset, _offset + skip_len);

	_offset += (skip_len+read_len); // buffer offset
	offset += (skip_len+read_len);	// disk offset
	if (r_req != NULL) {
	    auto req1 = new read1_req(&cv, &done);
	    r_req->run(req1);
	    std::unique_lock lk(m);
	    while (!done)
		cv.wait(lk);
	    r_req->release();
	}
    }
    memcpy(buf, buf2, len);
    free(buf2);
}

class read2_req : public trivial_request {
public:
    int refcnt = 0;
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    
    read2_req() { }
    ~read2_req() { }

    void add_ref() {
	std::unique_lock lk(m);
	refcnt++;
    }
    void run(request *unused) {
	std::unique_lock lk(m);
	started = true;
	if (refcnt == 0)
	    cv.notify_all();
    }
    void notify(request *child) {
	std::unique_lock lk(m);
	if (--refcnt == 0 && started) 
	    cv.notify_all();
    }
    void wait() {
	std::unique_lock lk(m);
	while (refcnt > 0)
	    cv.wait(lk);
    }
};

/* note that this leaks read cache request structures, because it 
 * doesn't call req->release()
 */
extern "C" void rcache_read2(read_cache *rcache, char *buf,
			    uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    std::condition_variable cv;

    smartiov iovs(buf2, len);

    auto req = new read2_req();
    
    for (size_t _offset = 0; _offset < len; ) {
	auto tmp = iovs.slice(_offset, len);
	auto [skip_len, read_len, r_req] =
	    rcache->async_readv(offset, &tmp);

	if (skip_len) 
	    iovs.zero(_offset, _offset + skip_len);

	_offset += (skip_len+read_len);
	offset += (skip_len+read_len);

	if (r_req != NULL) {
	    req->add_ref();
	    r_req->run(req);
	}
    }
    req->run(NULL);
    req->wait();
    memcpy(buf, buf2, len);
    free(buf2);
    delete req;
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
extern "C" void fakemap_update(_dbg *d, int base, int limit,
			       int obj, int offset)
{
    extmap::obj_offset oo = {obj,offset};
    d->obj_map.update(base, limit, oo);
    d->lsvd->set_completion(obj+1);
}
extern "C" void fakemap_reset(_dbg *d)
{
    d->obj_map.reset();
}

class aio_vreq {
public:
    enum lsvd_op op;
    smartiov   iovs;
    char      *buf;
    bool       done = false;
    rbd_completion_t c;
    std::mutex m;
    std::condition_variable cv;

    aio_vreq(enum lsvd_op op_, char *buf_, size_t *sizes, int n) {
	op = op_;
	buf = buf_;
	for (int i = 0; i < n; i++) {
	    auto ptr = aligned_alloc(512, sizes[i]*512);
	    iovs.push_back((iovec){ptr, sizes[i]*512});
	}
    }
    ~aio_vreq() {
	for (int i = 0; i < iovs.size(); i++)
	    free(iovs[i].iov_base);
    }
};

void aio_vreq_cb(rbd_completion_t c, void *arg) {
    auto r = (aio_vreq*)arg;
    std::unique_lock lk(r->m);
    r->done = true;
    r->cv.notify_one();
    rbd_aio_release(c);
}    

extern "C" void *launch_rbd_aio_writev(rbd_image_t image, uint64_t offset,
				      char *buf, size_t *sizes, int nsizes) {
    auto req = new aio_vreq(OP_WRITE, buf, sizes, nsizes);
    req->iovs.copy_in(buf);
    auto [iov,niov] = req->iovs.c_iov();
    
    rbd_completion_t c;
    rbd_aio_create_completion((void*)req, aio_vreq_cb, &c);
    req->c = c;
    rbd_aio_writev(image, iov, niov, offset, c);

    return (void*)req;
}
extern "C" void wait_rbd_aio_writev(void *r) {
    auto req = (aio_vreq*)r;
    std::unique_lock lk(req->m);
    while (!req->done)
	req->cv.wait(lk);
    lk.unlock();
    delete req;
}
 
extern "C" void *launch_rbd_aio_readv(rbd_image_t image, uint64_t offset,
				      char *buf, size_t *sizes, int nsizes) {
    auto req = new aio_vreq(OP_READ, buf, sizes, nsizes);
    auto [iov,niov] = req->iovs.c_iov();
    
    rbd_completion_t c;
    rbd_aio_create_completion((void*)req, aio_vreq_cb, &c);
    req->c = c;
    rbd_aio_readv(image, iov, niov, offset, c);

    return (void*)req;
}

extern "C" void wait_rbd_aio_readv(void *r) {
    auto req = (aio_vreq*)r;
    std::unique_lock lk(req->m);
    while (!req->done)
	req->cv.wait(lk);
    req->iovs.copy_out(req->buf);
    lk.unlock();
    delete req;
}

struct waiter {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};

void waiter_cb(rbd_completion_t c, void *arg) {
    auto w = (waiter*) arg;
    std::unique_lock lk(w->m);
    w->done = true;
    w->cv.notify_one();
    rbd_aio_release(c);
}

extern "C" int do_rbd_aio_writev(rbd_image_t image, const iovec *iov,
		      int iovcnt, uint64_t offset) {
    rbd_completion_t c;
    waiter w;
    
    rbd_aio_create_completion((void*)&w, waiter_cb, &c);
    rbd_aio_writev(image, iov, iovcnt, offset, c);
    std::unique_lock lk(w.m);
    while (!w.done)
	w.cv.wait(lk);
    return 0;
}

extern "C" int do_rbd_aio_readv(rbd_image_t image, const iovec *iov,
		      int iovcnt, uint64_t offset) {
    rbd_completion_t c;
    waiter w;
    
    rbd_aio_create_completion((void*)&w, waiter_cb, &c);
    rbd_aio_readv(image, iov, iovcnt, offset, c);
    std::unique_lock lk(w.m);
    while (!w.done)
	w.cv.wait(lk);
    return 0;
}

extern "C" void get_rbd_uuid(rbd_image_t image, uuid_t *uuid) {
    auto img = (rbd_image *)image;
    memcpy(uuid, img->xlate->uuid, sizeof(*uuid));
}

struct timelog {
    uint64_t loc : 8;
    uint64_t val : 48;
    uint64_t time;
};

struct timelog *tl;
std::atomic<int> tl_index;
int tl_max = 10000000;

#include <x86intrin.h>
void log_time(uint64_t loc, uint64_t value) {
    return;
    if (tl == NULL)
	tl = (struct timelog*)malloc(tl_max * sizeof(struct timelog));
    auto t = __rdtsc();
    auto i = tl_index++;
    if (i < tl_max) 
	tl[i] = {loc, value, t};
}

void save_log_time(void) {
    return;
    FILE *fp = fopen("/tmp/timelog", "wb");
    size_t bytes = tl_index * sizeof(struct timelog);
    fwrite(tl, bytes, 1, fp);
    fclose(fp);
}

/* random run-time debugging stuff, not used at the moment...
 */
#if 1
#include <zlib.h>
static std::map<int,uint32_t> sector_crc;
char zbuf[512];
static std::mutex zm;

void add_crc(sector_t sector, iovec *iov, int niovs) {
    std::unique_lock lk(zm);
    for (int i = 0; i < niovs; i++) {
	for (size_t j = 0; j < iov[i].iov_len; j += 512) {
	    const unsigned char *ptr = j + (unsigned char*)iov[i].iov_base;
	    sector_crc[sector] = (uint32_t)crc32(0, ptr, 512);
	    sector++;
	}
    }
}

void check_crc(sector_t sector, iovec *iov, int niovs, const char *msg) {
    std::unique_lock lk(zm);
    for (int i = 0; i < niovs; i++) {
	for (size_t j = 0; j < iov[i].iov_len; j += 512) {
	    const unsigned char *ptr = j + (unsigned char*)iov[i].iov_base;
	    if (sector_crc.find(sector) == sector_crc.end()) {
		assert(memcmp(ptr, zbuf, 512) == 0);
	    }
	    else {
		unsigned crc1 = 0, crc2 = 0;
		assert((crc1 = sector_crc[sector]) == (crc2 = crc32(0, ptr, 512)));
	    }
	    sector++;
	}
    }
}

void list_crc(sector_t sector, int n) {
    for (int i = 0; i < n; i++)
	printf("%ld %08x\n", sector+i, sector_crc[sector+i]);
}

size_t iovsum(const iovec *iov, int iovcnt) {
    int sum = 0;
    for (int i = 0; i < iovcnt; i++)
	sum += iov[i].iov_len;
    return sum;
}

extern "C" void noop(void) {
}

extern "C" int rbd_getmap(sector_t base, sector_t limit, rbd_image *img, int max, struct tuple *t) {
    getmap_s s = {0, max, t};
    img->xlate->getmap(base, limit, getmap_cb, (void*)&s);
    return s.i;
}

extern "C" void map_lookup(extmap::objmap *map, sector_t sector, int len) {
    sector_t base = sector, limit = base+len;
    auto it = map->lookup(base);
    if (it == map->end())
	printf("not found\n");
    else if (it->base() >= limit)
	printf("next entry: %d\n", (int)it->base());
    else {
	auto [_base, _limit, _ptr] = it->vals(base, limit);
	printf("%ld %ld %ld+%d\n", _base, _limit, _ptr.obj, _ptr.offset);
    }
}

#endif

#include <fcntl.h>
void getcpu(int pid, int tid, int &u, int &s) {
    if (tid == 0) 
	return;
    char procname[128];
    sprintf(procname, "/proc/%d/task/%d/stat", pid, tid);
    int fd = open(procname, O_RDONLY);
    char buf[512], *p = buf;
    read(fd, buf, sizeof(buf));
    close(fd);
    for (int i = 0; i < 13; i++)
	p = strchr(p, ' ') + 1;
    u = strtol(p, &p, 0);
    s = strtol(p, &p, 0);
}

int __dbg_wc_tid;
int __dbg_gc_tid;
int __dbg_fio_tid;
#include <sys/syscall.h>

int get_tid(void) {
    return syscall(SYS_gettid);
}

int __dbg_write1;		// writes launched
int __dbg_write2;		// writes completed
int __dbg_write3, __dbg_write4;	// objects written, completed

std::mutex *__dbg_wcache_m;
std::mutex *__dbg_xlate_m;
const char *__dbg_gc_state = "   -";
int __dbg_gc_reads;
int __dbg_gc_writes;
int __dbg_gc_deletes;
int __dbg_gc_n;

int __dbg_t_room;
int __dbg_w_room;

std::atomic<int> __dbg_in_lsvd;

bool read_lock(std::mutex *m) {
    if (!m)
	return false;
    bool val = m->try_lock();
    if (val)
	m->unlock();
    return !val;
}
bool read_lock(std::shared_mutex *m) {
    if (!m)
	return false;
    bool val = m->try_lock();
    if (val)
	m->unlock();
    return !val;
}
    
#include <sys/time.h>
struct timeval tv0;
double gettime(void) {
    if (tv0.tv_sec == 0)
	gettimeofday(&tv0, NULL);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec - tv0.tv_sec + (tv.tv_usec - tv0.tv_usec) / 1.0e6;
}

std::atomic<int> __dbg_wq_1;
std::atomic<int> __dbg_wq_2;
std::atomic<int> __dbg_wq_3;
std::atomic<int> __dbg_wq_4;

void debug_thread(rbd_image *img) {
    int pid = getpid();
    int write1 = __dbg_write1, write2 = __dbg_write2;
    int write3 = 0, write4 = 0;
    int gcr = 0, gcw = 0, gcd = 0;
    int uf=0,sf=0,uw=0,sw=0,ug=0,sg=0;
    int t_room = 0, w_room = 0;
    int xlate_m = 0, wcache_m = 0, obj_m = 0;
    double time0 = gettime();
    int in_lsvd = 0;
    
    while (!img->done) {
	int t1 = __dbg_write1, t2 = __dbg_write2;
	int t3 = __dbg_write3, t4 = __dbg_write4;

	int n1 = __dbg_gc_reads, n2 = __dbg_gc_writes, n3 = __dbg_gc_deletes;

	int uf_,sf_,uw_,sw_,ug_,sg_;
	getcpu(pid, __dbg_fio_tid, uf_, sf_);
	getcpu(pid, __dbg_wc_tid, uw_, sw_);
	getcpu(pid, __dbg_gc_tid, ug_, sg_);

	int q1 = __dbg_wq_1;
	int q2 = __dbg_wq_2;
	int q3 = __dbg_wq_3;
	int q4 = __dbg_wq_4;
	double time1 = gettime();
	printf("%.3f %- 4d\t%- 4d\t%- 4d\t%- 4d\t%d %d %d\t%s %d\t%d %d %d\t%d %d %d %d %d %d\t%d %d\t%d\t%d %d %d %d\n",
	       time1-time0,
	       t1-write1, t2-write2, t3-write3, t4-write4,
	       xlate_m, wcache_m, obj_m,
	       __dbg_gc_state, __dbg_gc_n, 
	       n1-gcr, n2-gcw, n3-gcd,
	       uf_-uf, sf_-sf, uw_-uw, sw_-sw, ug_-ug, sg_-sg,
	       w_room, t_room,
	       in_lsvd,
	       q1, q2, q3, q4);
	time0 = time1;
	write1 = t1; write2 = t2; write3 = t3; write4 = t4;
	gcr = n1; gcw = n2; gcd = n3;
	uf=uf_; sf=sf_; uw=uw_; sw=sw_; ug=ug_; sg=sg_;
	t_room = w_room = 0;
	xlate_m = wcache_m = obj_m = 0;
	in_lsvd = 0;
	for (int i = 0; i < 20 && !img->done; i++) {
	    t_room += __dbg_t_room;
	    w_room += __dbg_w_room;
	    xlate_m += read_lock(__dbg_xlate_m);
	    wcache_m += read_lock(__dbg_wcache_m);
	    obj_m += read_lock(&img->map_lock);
	    in_lsvd += (__dbg_in_lsvd > 0);
	    usleep(10000);
	}
    }
}
