/*
 * file:        lsvd.cc
 * description: userspace block-on-object layer with librbd interface
 * 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <unistd.h>
#include <fcntl.h>

#include <uuid/uuid.h>

#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <thread>

#include <algorithm>

#include <stack>
#include <queue>
#include <vector>
#include <map>

#include <string>
#include <cassert>

#include "journal.h"
#include "smartiov.h"
#include "extent.h"

#include "base_functions.h"
#include "backend.h"
#include "misc_cache.h"
#include "translate.h"
#include "request.h"
#include "nvme.h"
#include "read_cache.h"
#include "write_cache.h"
#include "file_backend.h"
#include "rados_backend.h"

#include "fake_rbd.h"
#include "image.h"

/* RBD "image" and completions are only used in this file, so we
 * don't break them out into a .h
 */

/* fake RBD image */

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

/* rbd_aio_req - state machine for rbd_aio_read, rbd_aio_write
 *
 * TODO: fix this. I merged separate read & write classes in the
 * ugliest possible way, but it works...
 */
class rbd_aio_req : public request {
    fake_rbd_image   *img;
    lsvd_completion  *p;
    char             *buf;
    char             *aligned_buf;
    uint64_t          offset;
    size_t            len;
    lsvd_op           op;
    smartiov          data_iovs;

    /* note - 'complete' = (n_req == 0) */
    std::atomic<int>  n_req = 0;
    std::atomic<bool> launched = false;
    bool              released = false;
    
    bool              complete = false; // write only

    std::mutex        m;
    std::condition_variable cv;

    void notify_w(request *unused) {
        int blocks = div_round_up(len, 4096);
        img->wcache->release_room(blocks);
        
        complete = true;
        if (p != NULL) 
            p->complete(len);

        if (released)
            delete this;
        else
            cv.notify_all();
    }
    
    void notify_r(request *child) {
	if (child)
	    child->release();
        if (--n_req > 0)
            return;
        if (!launched)
            return;
        
        if (aligned_buf != buf) 
            memcpy(buf, aligned_buf, len);

        if (p != NULL) 
            p->complete(len);

        if (released)
            delete this;
        else 
            cv.notify_all();
    }
    
    void run_w() {
        if (!aligned(buf, 512)) {
            aligned_buf = (char*)aligned_alloc(512, len);
            memcpy(aligned_buf, buf, len);
        }
        data_iovs.push_back((iovec){aligned_buf, len});
        int blocks = div_round_up(len, 4096);
        img->wcache->get_room(blocks);
        img->wcache->writev(this);
    }
    
    void run_r() {
        if (!aligned(buf, 512))
            aligned_buf = (char*)aligned_alloc(512, len);

        /* we're not done until n_req == 0 && launched == true
         */
        char *_buf = aligned_buf;       // read and increment this
        size_t _len = len;              // and this
	
        while (_len > 0) {
            auto [skip,wait,rreq] =
                img->wcache->async_read(offset, _buf, _len);
	    if (rreq != NULL) {
		n_req++;
		rreq->run(this);
	    }

            _len -= skip;
            while (skip > 0) {
                auto [skip2, wait2, req] =
                    img->rcache->async_read(offset, _buf, skip);
		if (req) {
		    n_req++;
		    req->run(this);
		}
                memset(_buf, 0, skip2);
                skip -= (skip2 + wait2);
                _buf += (skip2 + wait2);
                offset += (skip2 + wait2);
            }
            _buf += wait;
            _len -= wait;
            offset += wait;
	}
        launched.store(true);
        if (n_req == 0)
            notify(NULL);
    }
    
public:
    rbd_aio_req(lsvd_op op_, fake_rbd_image *img_, lsvd_completion *p_,
		char *buf_, uint64_t offset_, size_t len_) {
	op = op_;
	img = img_;
	p = p_;
	buf = buf_;
	offset = offset_;
	len = len_;
	aligned_buf = buf;
    }
    ~rbd_aio_req() {
	if (aligned_buf != buf)
	    free(aligned_buf);
    }

    sector_t lba() { return offset / 512; }
    smartiov *iovs() { return &data_iovs; }

    /* note that there's no child request until read cache is updated
     * to use request/notify model.
     */
    void notify(request *child) {
	if (op == OP_READ)
	    notify_r(child);
	else
	    notify_w(child);
    }

    bool is_done() {
	if (op == OP_READ)
	    return launched && n_req <= 0;
	else
	    return complete;
    }

    /* TODO: this is really gross. To properly fix it I need to integrate this
     * with rbd_aio_completion and use its release() method
     */
    void wait() {
	std::unique_lock lk(m);
	while ((op == OP_READ && (!launched || n_req > 0)) ||
	       (op == OP_WRITE && !complete))
	    cv.wait(lk);
	lk.unlock();
	release();
    }

    void release() {
	released = true;
	if ((op == OP_READ && n_req == 0) ||
	    (op == OP_WRITE && complete))
		delete this;
    }

    void run(request *parent /* unused */) {
	if (op == OP_READ)
	    run_r();
	else
	    run_w();
    }

    static void aio_read_cb(void *ptr) {
	auto req = (rbd_aio_req *)ptr;
	req->notify(NULL);
    }
};

extern "C" int rbd_aio_read(rbd_image_t image, uint64_t offset,
			    size_t len, char *buf, rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    auto p = (lsvd_completion*)c;
    p->fri = fri;

    auto req = new rbd_aio_req(OP_READ, fri, p, buf, offset, len);
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

extern "C" int rbd_aio_write(rbd_image_t image, uint64_t offset, size_t len,
			     const char *buf, rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = fri;

    auto req = new rbd_aio_req(OP_WRITE, fri, p, (char*)buf, offset, len);
    req->run(NULL);
    return 0;
}

/* note that rbd_aio_read handles aligned bounce buffers for us
 */
extern "C" int rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    auto req = new rbd_aio_req(OP_READ, fri, NULL, buf, off, len);
    req->run(NULL);
    req->wait();
    return 0;
}

extern "C" int rbd_write(rbd_image_t image, uint64_t off, size_t len, const char *buf)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    auto req = new rbd_aio_req(OP_WRITE, fri, NULL, (char*)buf, off, len);
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

std::pair<std::string,std::string> split_string(std::string s,
						std::string delim) {
    auto i = s.find(delim);
    return std::pair(s.substr(0,i), s.substr(i+delim.length()));
}

extern "C" int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image,
			const char *snap_name)
{
    int rv;
    auto [nvme, obj] = split_string(std::string(name), ",");
    bool rados = (obj.substr(0,6) == "rados:");
    auto fri = new fake_rbd_image;
    
    if (rados)
	fri->io = make_rados_backend();
    else
	fri->io = new file_backend();
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
	(void)_tmp;		// suppress unused warning
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
    
    fri->wcache = make_write_cache(js->write_super, fd, fri->lsvd,
				   n_wc_threads);
    fri->rcache = make_read_cache(js->read_super, fd, false,
				  fri->lsvd, fri->omap, fri->io);

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

