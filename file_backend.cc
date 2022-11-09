/*
 * file:        file_backend.cc
 * description: local filesystem-based object backend
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <libaio.h>

#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <random>

#include "lsvd_types.h"
#include "backend.h"
#include "request.h"
#include "smartiov.h"
#include "io.h"
#include "misc_cache.h"

bool __lsvd_dbg_be_delay = false;
long __lsvd_dbg_be_seed = 1;
int __lsvd_dbg_be_threads = 10;
int __lsvd_dbg_be_delay_ms = 5;
bool __lsvd_dbg_rename = false;
static std::mt19937_64 rng;

class file_backend_req;

class file_backend : public backend {
    void worker(thread_pool<file_backend_req*> *p);
    std::mutex m;
    
public:
    file_backend();
    ~file_backend();

    /* see backend.h 
     */
    int write_object(const char *name, iovec *iov, int iovcnt);
    int read_object(const char *name, iovec *iov, int iovcnt, size_t offset);
    int delete_object(const char *name);
    
    /* async I/O
     */
    request *make_write_req(const char *name, iovec *iov, int iovcnt);
    request *make_read_req(const char *name, size_t offset,
                           iovec *iov, int iovcnt);
    request *make_read_req(const char *name, size_t offset,
                           char *buf, size_t len);
    void kill(void);
    thread_pool<file_backend_req*> workers;
};

/* trivial methods 
 */
int file_backend::write_object(const char *name, iovec *iov, int iovcnt) {
    int fd;
    if ((fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0777)) < 0)
	return -1;
    auto val = writev(fd, iov, iovcnt);
    close(fd);
    return val < 0 ? -1 : 0;
}

int file_backend::read_object(const char *name, iovec *iov, int iovcnt,
				  size_t offset) {
    int fd;
    if ((fd = open(name, O_RDONLY)) < 0)
	return -1;
    if (fd < 0)
	return -1;
    int val = lseek(fd, offset, SEEK_SET);
    if (val >= 0)
	val = readv(fd, iov, iovcnt);
    close(fd);
    return val < 0 ? -1 : 0;
}

int file_backend::delete_object(const char *name) {
    int rv;
    if (__lsvd_dbg_rename) {
	std::string newname = std::string(name) + ".bak";
	rv = rename(name, newname.c_str());
    }
    else
	rv = unlink(name);
    return rv < 0 ? -1 : 0;
}

class file_backend_req : public request {
    enum lsvd_op    op;
    smartiov        _iovs;
    size_t          offset;
    std::string     name;
    request        *parent = NULL;
    file_backend   *be;
    int             fd = -1;
    
public:
    file_backend_req(enum lsvd_op op_, const char *name_,
		     iovec *iov, int iovcnt, size_t offset_,
		     file_backend *be_) : _iovs(iov, iovcnt), name(name_) {
	op = op_;
	offset = offset_;
	be = be_;
    }
    ~file_backend_req() {}

    void      wait() {}		   // TODO: ?????
    void      run(request *parent);
    void      notify(request *child);
    void      release() {};

    static void rw_cb_fn(void *ptr) {
	auto req = (file_backend_req*)ptr;
	req->notify(NULL);
    }

    void exec(void) {
	int mode = (op == OP_READ) ? O_RDONLY : O_RDWR | O_CREAT | O_TRUNC;
	if ((fd = open(name.c_str(), mode, 0777)) < 0)
	    throw("file object error");

	auto [iov,niovs] = _iovs.c_iov();
	if (op == OP_READ) 
	    preadv(fd, iov, niovs, offset);
	else
	    pwritev(fd, iov, niovs, offset);
	notify(NULL);
    }

    void kill(void) {
	if (fd != -1)
	    close(fd);
    }
};


/* methods that depend on file_backend_req
 */
file_backend::file_backend() : workers(&m) {
    rng.seed(__lsvd_dbg_be_seed);
    for (int i = 0; i < __lsvd_dbg_be_threads; i++) 
	workers.pool.push(std::thread(&file_backend::worker, this, &workers));
}

file_backend::~file_backend() {
}

void file_backend::worker(thread_pool<file_backend_req*> *p) {
    pthread_setname_np(pthread_self(), "file_worker");
    while (p->running) {
	file_backend_req *req;
	if (!p->get(req)) 
	    return;
	if (__lsvd_dbg_be_delay) {
	    std::uniform_int_distribution<int> uni(0,__lsvd_dbg_be_delay_ms*1000);
	    useconds_t usecs = uni(rng);
	    usleep(usecs);
	}
	req->exec();
    }
}

static void close_req(file_backend_req *req) {
    req->kill();
}

void file_backend::kill(void) {
    workers.kill(close_req);
}

/* TODO: run() ought to return error/success
 */
void file_backend_req::run(request *parent_) {
    parent = parent_;
    be->workers.put(this);
}

/* TODO: this assumes no use of wait/release
 */
void file_backend_req::notify(request *unused) {
    close(fd);
    parent->notify(this);
    delete this;
}

request *file_backend::make_write_req(const char*name, iovec *iov, int niov) {
    assert(access(name, F_OK) != 0);
    return new file_backend_req(OP_WRITE, name, iov, niov, 0, this);
}

request *file_backend::make_read_req(const char *name, size_t offset,
				     iovec *iov, int iovcnt) {
    return new file_backend_req(OP_READ, name, iov, iovcnt, offset, this);
}

request *file_backend::make_read_req(const char *name, size_t offset,
				     char *buf, size_t len) {
    iovec iov = {buf, len};
    return new file_backend_req(OP_READ, name, &iov, 1, offset, this);
}

backend *make_file_backend(void) {
    return new file_backend;
}
