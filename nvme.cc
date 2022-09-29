#include <libaio.h>
#include <uuid/uuid.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <atomic>
#include <map>
#include <condition_variable>
#include <thread>
#include <stack>
#include <queue>
#include <cassert>
#include <shared_mutex>

#include <sys/uio.h>

#include <mutex>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>

#include "base_functions.h"

#include "journal2.h"
#include "smartiov.h"
#include "objects.h"
#include "extent.h"
#include "misc_cache.h"
#include "backend.h"
#include "io.h"
#include "translate.h"
#include "request.h"

#include "nvme.h"
#include "write_cache.h"
#include "send_write_request.h"

class nvme_impl;

class nvme_request : public request {
    e_iocb     *eio;		// this is self-deleting. TODO: fix?
    smartiov   *_iovs;
    size_t      ofs;
    int         t;
    nvme_impl  *nvme_ptr;
    request    *parent;

    bool        released = false;
    bool        complete = false;
    std::mutex  m;
    std::condition_variable cv;
    
public:
    nvme_request(smartiov *iov, size_t offset, int type, nvme_impl* nvme_w);
    ~nvme_request();

    sector_t lba();
    smartiov *iovs();
    bool is_done(void);
    void wait();
    void run(request *parent);
    void notify(request *child);
    void release();
};

class nvme_impl : public nvme {
public:
    int fd;
    bool e_io_running = false;
    std::thread e_io_th;
    io_context_t ioctx;

    nvme_impl(int fd_, const char *name_) {
	fd = fd_;
	e_io_running = true;
	io_queue_init(64, &ioctx);
	e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name_);
    }

    ~nvme_impl() {
	e_io_running = false;
	e_io_th.join();
	io_queue_release(ioctx);
    }

    request* make_write_request(smartiov *iov, size_t offset) {
	assert(offset != 0);
	auto req = new nvme_request(iov, offset, WRITE_REQ, this);
	return (request*) req;
    }

    request* make_read_request(smartiov *iov, size_t offset) {
	auto req = new nvme_request(iov, offset, READ_REQ, this);
	return (request*) req;
    }
};

nvme *make_nvme(int fd, const char* name) {
    return (nvme*) new nvme_impl(fd, name);
}

/* ------- nvme_request implementation -------- */

void call_send_request_notify(void *parent)
{
    nvme_request *r = (nvme_request*) parent;
    r->notify(NULL);
}

nvme_request::nvme_request(smartiov *iov, size_t offset,
			   int type, nvme_impl* nvme_w) {
    eio = new e_iocb;
    _iovs = iov;
    ofs = offset;
    t = type;
    nvme_ptr = nvme_w;
}

void nvme_request::run(request* parent_) {
    parent = parent_;

    if (t == WRITE_REQ) {
	e_io_prep_pwritev(eio, nvme_ptr->fd, _iovs->data(), _iovs->size(),
			  ofs, call_send_request_notify, this);
	e_io_submit(nvme_ptr->ioctx, eio);

    } else if (t == READ_REQ) {
	e_io_prep_preadv(eio, nvme_ptr->fd, _iovs->data(), _iovs->size(),
			 ofs, call_send_request_notify, this);
	e_io_submit(nvme_ptr->ioctx, eio);
    } else
	assert(false);
}

void nvme_request::notify(request *child) {
    parent->notify(this);
    std::unique_lock lk(m);
    complete = true;
    if (released)
	delete this;
    else
	cv.notify_one();
}

void nvme_request::wait() {
    std::unique_lock lk(m);
    while (!complete)
	cv.wait(lk);
}

void nvme_request::release() {
    released = true;
    if (complete)		// TODO: atomic swap?
	delete this;
}

nvme_request::~nvme_request() {}

bool nvme_request::is_done() { return complete; }
sector_t nvme_request::lba() { return 0; } // no one needs this
smartiov *nvme_request::iovs() { return NULL; }

