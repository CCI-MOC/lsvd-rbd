/*
 * file:        lsvd_rbd.cc
 * description: userspace block-on-object layer with librbd interface
 * 
 * Copyright 2021, 2022 Peter Desnoyers
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <rados/librados.h>
#include <libaio.h>
#include <uuid/uuid.h>

#include <mutex>
#include <shared_mutex>
#include <stack>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <string>
#include <map>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>

#include "journal2.h"
#include "smartiov.h"
#include "extent.h"
#include "objects.h"

#include <sys/uio.h>

#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>

#include "base_functions.h"
#include "backend.h"
#include "misc_cache.h"
#include "translate.h"
#include "io.h"
#include "request.h"
#include "nvme.h"
#include "read_cache.h"
#include "write_cache.h"
#include "file_backend.h"
#include "rados_backend.h"

#include "fake_rbd.h"
#include "lsvd_debug.h"

/* RBD "image" and completions are only used in this file, so we
 * don't break them out into a .h
 */

/* fake RBD image */

struct lsvd_completion;

struct event_socket {
    int socket;
    int type;
public:
    event_socket(): socket(-1), type(0) {}
    bool is_valid() const { return socket != -1; }
    int init(int fd, int t) {
	socket = fd;
	type = t;
	return 0;
    }
    int notify() {
	int rv;
	switch (type) {
	case EVENT_TYPE_PIPE:
	{
	    char buf[1] = {'i'}; // why 'i'???
	    rv = write(socket, buf, 1);
	    rv = (rv < 0) ? -errno : 0;
	    break;
	}
	case EVENT_TYPE_EVENTFD:
	{
	    uint64_t value = 1;
	    rv = write(socket, &value, sizeof (value));
	    rv = (rv < 0) ? -errno : 0;
	    break;
	}
	default:
	    rv = -1;
	}
	return rv;
    }
};

struct fake_rbd_image {
    std::mutex   m;
    backend     *io;
    objmap      *omap;
    translate   *lsvd;
    write_cache *wcache;
    read_cache  *rcache;
    ssize_t      size;          // bytes
    int          fd;            // cache device
    j_super     *js;            // cache page 0

    event_socket ev;
    std::queue<rbd_completion_t> completions;
};

/* RBD-level completion structure 
 */
struct lsvd_completion {
public:
    fake_rbd_image *fri;
    rbd_callback_t cb;
    void *arg;
    int retval;
    bool done = false;
    std::atomic<bool> released {false};
    std::mutex m;
    std::condition_variable cv;

    std::atomic<int> n = 0;
    iovec iov;                  // occasional use only
    
    lsvd_completion(rbd_callback_t cb_, void *arg_) : cb(cb_), arg(arg_) {}

    /* see Ceph AioCompletion::complete
     */
    void complete(int val) {
	retval = val;
	if (cb)
	    cb((rbd_completion_t)this, arg);
	if (fri->ev.is_valid()) {
	    {
		std::unique_lock lk(fri->m);
		fri->completions.push(this);
	    }
	    fri->ev.notify();
	}
	
	done = true;
	if (released)
	    delete this;
	else {
	    std::unique_lock lk(m);
	    cv.notify_all();
	}
    }

    /* the Ceph folks *really* want to make sure users don't
     * release twice
     */
    void release() {
	//printf("release %p\n", this);
	bool old_released = released.exchange(true);
	assert(!old_released);
	if (done)
	    delete this;
    }
};

extern "C" int rbd_poll_io_events(rbd_image_t image,
				  rbd_completion_t *comps, int numcomp)
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
    return fri->ev.init(fd, type);
}

extern "C" int rbd_aio_create_completion(void *cb_arg,
					 rbd_callback_t complete_cb,
					 rbd_completion_t *c)
{
    lsvd_completion *p = new lsvd_completion(complete_cb, cb_arg);
    *c = (rbd_completion_t)p;
    return 0;
}

extern "C" void rbd_aio_release(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->release();
}

extern "C" int rbd_aio_discard(rbd_image_t image, uint64_t off,
			       uint64_t len, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = (fake_rbd_image*)image;

    /* TODO: implement
     */

    p->complete(0);
    return 0;
}

extern "C" int rbd_aio_flush(rbd_image_t image, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = (fake_rbd_image*)image;

    /* TODO: implement
     */

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

/* aio_read_req - state machine for rbd_aio_read
 */
class aio_read_req : public request {
    fake_rbd_image   *img;
    lsvd_completion  *p;
    char             *buf;
    char             *aligned_buf;
    uint64_t          offset;
    size_t            len;
    std::atomic<int>  n_req = 0;
    std::atomic<bool> finished = false;

    std::mutex        m;
    std::condition_variable cv;
    
public:
    aio_read_req(fake_rbd_image *img_, lsvd_completion *p_,
		 char *buf_, uint64_t offset_, size_t len_) :
	img(img_), p(p_), buf(buf_), offset(offset_), len(len_) {
	aligned_buf = buf;
    }

    bool is_done() { return finished && n_req <= 0; }
    sector_t lba() { return 0; }
    smartiov *iovs() { return NULL; }
    
    void notify() {
	if (--n_req > 0)
	    return;
	if (!finished)
	    return;
	
	if (aligned_buf != buf) {
	    memcpy(buf, aligned_buf, len);
	    free(aligned_buf);
	}
	if (p != NULL) {
	    p->complete(len);
	    delete this;
	} else {
	    std::unique_lock lk(m);
	    cv.notify_all();
	}
    }

    void wait() {
	std::unique_lock lk(m);
	while (!finished || n_req > 0)
	    cv.wait(lk);
	lk.unlock();
	delete this;
    }
    
    void run(request *parent /* unused */) {
	if (!aligned(buf, 512))
	    aligned_buf = (char*)aligned_alloc(512, len);

	/* we're not done until n_req == 0 && finished == true
	 */
	char *_buf = aligned_buf;	// read and increment this
	size_t _len = len;		// and this

	while (_len > 0) {
	    n_req++;
	    auto [skip,wait] =
		img->wcache->async_read(offset, _buf, _len,
					aio_read_cb, (void*)this);
	    if (wait == 0)
		n_req--;

	    _len -= skip;
	    while (skip > 0) {
		n_req++;
		auto [skip2, wait2] =
		    img->rcache->async_read(offset, _buf, skip,
					    aio_read_cb, (void*)this);
		if (wait2 == 0)
		    n_req--;

		memset(_buf, 0, skip2);
		skip -= (skip2 + wait2);
		_buf += (skip2 + wait2);
		offset += (skip2 + wait2);
	    }
	    _buf += wait;
	    _len -= wait;
	    offset += wait;
	}

	finished.store(true);
	if (n_req == 0)
	    notify();
    }

    static void aio_read_cb(void *ptr) {
	auto req = (aio_read_req *)ptr;
	req->notify();
    }
};


extern "C" int rbd_aio_read(rbd_image_t image, uint64_t offset,
			    size_t len, char *buf, rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    auto p = (lsvd_completion*)c;
    p->fri = fri;

    auto req = new aio_read_req(fri, p, buf, offset, len);
    req->run(NULL);
    return 0;
}

extern "C" int rbd_aio_readv(rbd_image_t image, const iovec *iov,
			     int iovcnt, uint64_t off, rbd_completion_t c)
{
    /* TODO */
    return 0;
}

extern "C" int rbd_aio_writev(rbd_image_t image, const struct iovec *iov,
			      int iovcnt, uint64_t off, rbd_completion_t c)
{
    /* TODO */
    return 0;
}

/* aio_write_req - state machine for rbd_aio_write
 */
class aio_write_req : public request {
    fake_rbd_image   *img;
    lsvd_completion  *p;
    const char       *buf;
    char             *aligned_buf = NULL;
    uint64_t          offset;
    size_t            len;
    smartiov          data_iovs;
    bool              finished = false;
    std::mutex        m;
    std::condition_variable cv;
    
public:
    aio_write_req(fake_rbd_image *img_, lsvd_completion *p_,
		  const char *buf_, uint64_t offset_, size_t len_) :
	img(img_), p(p_), buf(buf_), offset(offset_), len(len_) {
	aligned_buf = (char*)buf;
    }

    void notify() {
	int blocks = div_round_up(len, 4096);
	img->wcache->release_room(blocks);
	
	if (aligned_buf != buf)
	    free(aligned_buf);
	if (p != NULL) {
	    //printf("complete %p\n", p);
	    p->complete(len);
	    delete this;
	} else {
	    std::unique_lock lk(m);
	    finished = true;
	    cv.notify_all();
	}
    }

    /* for synchronous writes
     */
    void wait() {
	std::unique_lock lk(m);
	while (!finished)
	    cv.wait(lk);
	lk.unlock();
	delete this;
    }

    bool is_done() { return finished; }
    sector_t lba() { return offset / 512; }
    smartiov *iovs() { return &data_iovs; }
    
    /* note that this is a lot simpler than read, because
     * the write cache takes everything in a single chunk.
     */
    void run(request *parent /* unused */) {
	if (!aligned(buf, 512)) {
	    aligned_buf = (char*)aligned_alloc(512, len);
	    memcpy(aligned_buf, buf, len);
	}
	data_iovs.push_back((iovec){aligned_buf, len});
	int blocks = div_round_up(len, 4096);
	img->wcache->get_room(blocks);
	img->wcache->writev(this);
    }
};

extern "C" int rbd_aio_write(rbd_image_t image, uint64_t offset, size_t len,
			     const char *buf, rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = fri;

    auto req = new aio_write_req(fri, p, buf, offset, len);
    req->run(NULL);
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
    fake_rbd_image *fri = (fake_rbd_image*)image;
    auto req = new aio_read_req(fri, NULL, buf, off, len);
    req->run(NULL);
    req->wait();
    return 0;
}

extern "C" int rbd_write(rbd_image_t image, uint64_t off, size_t len, const char *buf)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    auto req = new aio_write_req(fri, NULL, buf, off, len);
    req->run(NULL);
    req->wait();
    return 0;
}

extern "C" int rbd_aio_wait_for_complete(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    std::unique_lock lk(p->m);
    while (!p->done)
	p->cv.wait(lk);
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
    fri->lsvd = make_translate(fri->io, fri->omap);
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
    free(fri->js);
    delete fri;
    
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
extern "C" int xlate_open(char *name, int n, bool flushthread, void **p)
{
    backend *io = new file_backend(name);
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
extern "C" void wcache_reset(write_cache *wcache)
{
    wcache->reset();
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
