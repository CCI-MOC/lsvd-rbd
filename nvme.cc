/*
 * file:        nvme.cc
 * description: implementation of read/write requests to local SSD
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <libaio.h>
#include <unistd.h>
#include <sys/uio.h>
#include <uuid/uuid.h>
#include <signal.h>

#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>

#include "lsvd_types.h"
#include "smartiov.h"
#include "extent.h"
#include "misc_cache.h"
#include "backend.h"
#include "io.h"
#include "request.h"

#include "nvme.h"

class nvme_impl;

class nvme_request : public request {
public:
    e_iocb      eio;
    smartiov    _iovs;
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
    nvme_request(char *buf, size_t len, size_t offset, int type,
		 nvme_impl* nvme_w);
    ~nvme_request();

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

    static void sighandler(int sig) {
	pthread_exit(NULL);
    }
    
    nvme_impl(int fd_, const char *name_) {
	fd = fd_;
	e_io_running = true;
	io_queue_init(64, &ioctx);
	signal(SIGUSR2, sighandler);
	e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name_);
    }

    ~nvme_impl() {
	e_io_running = false;
	//e_io_th.join();
	//pthread_cancel(e_io_th.native_handle());
	pthread_kill(e_io_th.native_handle(), SIGUSR2);
	e_io_th.join();
	io_queue_release(ioctx);
    }

    int read(void *buf, size_t count, off_t offset) {
	return pread(fd, buf, count, offset);
    }
    
    int write(const void *buf, size_t count, off_t offset) {
	return pwrite(fd, buf, count, offset);
    }

    int writev(const struct iovec *iov, int iovcnt, off_t offset) {
	return pwritev(fd, iov, iovcnt, offset);
    }
    
    int readv(const struct iovec *iov, int iovcnt, off_t offset) {
	return preadv(fd, iov, iovcnt, offset);
    }

    request* make_write_request(smartiov *iov, size_t offset) {
	assert(offset != 0);
	auto req = new nvme_request(iov, offset, WRITE_REQ, this);
	return (request*) req;
    }

    request* make_write_request(char *buf, size_t len, size_t offset) {
	assert(offset != 0);
	auto req = new nvme_request(buf, len, offset, WRITE_REQ, this);
	return (request*) req;
    }
    
    request* make_read_request(smartiov *iov, size_t offset) {
	auto req = new nvme_request(iov, offset, READ_REQ, this);
	return (request*) req;
    }

    request* make_read_request(char *buf, size_t len, size_t offset) {
	auto req = new nvme_request(buf, len, offset, READ_REQ, this);
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
			   int type, nvme_impl* nvme_w) : _iovs(iov->data(),
								iov->size()) {
    ofs = offset;
    t = type;
    nvme_ptr = nvme_w;
}

nvme_request::nvme_request(char *buf, size_t len, size_t offset,
			   int type, nvme_impl* nvme_w) {
    _iovs.push_back((iovec){buf, len});
    ofs = offset;
    t = type;
    nvme_ptr = nvme_w;
}

void nvme_request::run(request* parent_) {
    parent = parent_;
    if (t == WRITE_REQ) 
	e_io_prep_pwritev(&eio, nvme_ptr->fd, _iovs.data(), _iovs.size(),
			  ofs, call_send_request_notify, this);
    else
	e_io_prep_preadv(&eio, nvme_ptr->fd, _iovs.data(), _iovs.size(),
			 ofs, call_send_request_notify, this);
    e_io_submit(nvme_ptr->ioctx, &eio);
}

void nvme_request::notify(request *child) {
    if (parent)
	parent->notify(this);

    std::unique_lock lk(m);
    complete = true;
    cv.notify_one();
    if (released) {
	lk.unlock();
	delete this;
    }
}

void nvme_request::wait() {
    std::unique_lock lk(m);
    while (!complete)
	cv.wait(lk);
}

void nvme_request::release() {
    std::unique_lock lk(m);
    released = true;
    if (complete) {		// TODO: atomic swap?
	lk.unlock();
	delete this;
    }
}

nvme_request::~nvme_request() {}
