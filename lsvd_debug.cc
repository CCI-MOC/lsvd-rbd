#include <unistd.h>

#include <queue>
#include <map>

#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>

#include "lsvd_types.h"
#include "smartiov.h"
#include "fake_rbd.h"
#include "image.h"

#include "extent.h"
#include "journal.h"
#include "objects.h"
#include "request.h"

#include "misc_cache.h"
#include "nvme.h"
#include "base_functions.h"

#include "translate.h"
#include "read_cache.h"
#include "write_cache.h"
#include "file_backend.h"
#include "rados_backend.h"

/* types used to interface with some debug functions - must
 * match ctypes definitions in lsvd_types.py
 */
struct _dbg {
public:
    int type = 0;
    translate   *lsvd;
    write_cache *wcache;
    objmap      *omap;
    read_cache  *rcache;
    backend     *io;
    _dbg(int _t, translate *_l, write_cache *_w, objmap *_o,
         read_cache *_r, backend *_io) : type(_t), lsvd(_l), wcache(_w),
        omap(_o), rcache(_r), io(_io) {}
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
extern "C" int xlate_open(char *name, int n, bool flushthread, void **p)
{
    backend *io = new file_backend();
    objmap *omap = new objmap();
    translate *lsvd = make_translate(io, omap);
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
int getmap_cb(void *ptr, int base, int limit, int obj, int offset)
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

extern int batch_seq(translate*);
extern "C" int xlate_seq(_dbg *d)
{
    return batch_seq(d->lsvd);
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
    auto wcache = make_write_cache(blkno, fd, d->lsvd, 2);
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
        auto [skip_len, read_len, rreq] = wcache->async_read(offset, _buf, _len);

        memset(_buf, 0, skip_len);
        _buf += (skip_len + read_len);
        _len -= (skip_len + read_len);
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
    fake_rbd_image *fri = new fake_rbd_image;
    fri->wcache = wcache;
    wcache_img_write(fri, buf, offset, len);
    delete fri;
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

class trivial_req : public request {
public:
    trivial_req() {}
    ~trivial_req() {}
    virtual void notify(request *child) = 0;
    sector_t lba() { return 0; }
    smartiov *iovs() { return NULL; }
    bool is_done() { return true; }
    void wait() {}
    void run(request *parent) {}
    void release() {}
};

char logbuf[1024], *p_log = logbuf;
extern "C" int get_logbuf(char *buf) {
    memcpy(buf, logbuf, p_log - logbuf);
    return p_log - logbuf;
}

class read1_req : public request {
    std::condition_variable *cv;
    bool *done;
    
public:
    read1_req(std::condition_variable *cv_, bool *done_) {
	cv = cv_;
	done = done_;
    }
    ~read1_req() { printf("read1 delete\n"); }

    void notify(request *child) {
	p_log += sprintf(p_log, "read1 notify %p\n", this);
	*done = true;
	cv->notify_all();
    }

    sector_t lba() { return 0; }
    smartiov *iovs() { return NULL; }
    bool is_done() { return true; }
    void wait() {}
    void run(request *parent) {}
    void release() {}
};

/* note that this leaks read cache request structures, because it 
 * doesn't call req->release()
 */
extern "C" void rcache_read(read_cache *rcache, char *buf,
			    uint64_t offset, uint64_t len)
{
    char *buf2 = (char*)aligned_alloc(512, len); // just assume it's not
    int _len = len;
    std::mutex m;
    std::condition_variable cv;
    
    for (char *_buf = buf2; _len > 0; ) {
	bool done = false;
	auto [skip_len, read_len, r_req] =
	    rcache->async_read(offset, _buf, _len);
	
	memset(_buf, 0, skip_len);
	_buf += (skip_len+read_len);
	_len -= (skip_len+read_len);
	offset += (skip_len+read_len);
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

/* note that this leaks read cache request structures, because it 
 * doesn't call req->release()
 */
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
	auto cb_req = new callback_req(call_wrapped, closure);
	auto [skip_len, read_len, r_req] =
	    rcache->async_read(offset, _buf, _len);
	
	memset(_buf, 0, skip_len);
	_buf += (skip_len+read_len);
	_len -= (skip_len+read_len);
	offset += (skip_len+read_len);
	
	if (r_req != NULL)
	    r_req->run(cb_req);
	else {
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
