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
#include "io.h"
#include "read_cache.h"

/* the read cache is:
 * 1. indexed by obj/offset[*], not LBA
 * 2. stores aligned 64KB blocks 
 * [*] offset is in units of 64KB blocks
 */


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

    
