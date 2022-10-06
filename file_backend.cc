/*
 * file:        file_backend.cc
 * description: local filesystem-based object backend
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>

#include <fcntl.h>
#include <unistd.h>

#include "file_backend.h"
#include "io.h"


file_backend::file_backend() {
    e_io_running = true;
    io_queue_init(64, &ioctx);
    const char *name = "file_backend_cb";
    e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
}

file_backend::~file_backend() {
    e_io_running = false;
    e_io_th.join();
    io_queue_release(ioctx);
}

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
    if (unlink(name) < 0)
	return -1;
    return 0;
}

class file_backend_req : public request {
    enum lsvd_op    op;
    smartiov        _iovs;
    size_t          offset;
    std::string     name;
    io_context_t    ioctx;
    request        *parent = NULL;
    e_iocb         *eio;    // this is self-deleting. TODO: fix io.cc?
    int             fd;
    
public:
    file_backend_req(enum lsvd_op op_, const char *name_,
		     iovec *iov, int iovcnt, size_t offset_,
		     io_context_t ioctx_) : _iovs(iov, iovcnt), name(name_) {
	op = op_;
	offset = offset_;
	ioctx = ioctx_;
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
};

/* TODO: run() ought to return error/success
 */
void file_backend_req::run(request *parent_) {
    parent = parent_;
    eio = new e_iocb;
    int mode = (op == OP_READ) ? O_RDONLY : O_RDWR | O_CREAT | O_TRUNC;
    if ((fd = open(name.c_str(), mode, 0777)) < 0)
	throw("file object error");

    auto [iov,iovcnt] = _iovs.c_iov();
    if (op == OP_WRITE)
	e_io_prep_pwritev(eio, fd, iov, iovcnt, offset, rw_cb_fn, this);
    else if (op == OP_READ) 
	e_io_prep_preadv(eio, fd, iov, iovcnt, offset, rw_cb_fn, this);
    else
	assert(false);
    e_io_submit(ioctx, eio);
}

/* TODO: this assumes no use of wait/release
 */
void file_backend_req::notify(request *unused) {
    close(fd);
    parent->notify(this);
    delete this;
}

request *file_backend::make_write_req(const char*name, iovec *iov, int niov) {
    return new file_backend_req(OP_WRITE, name, iov, niov, 0, ioctx);
}

request *file_backend::make_read_req(const char *name, size_t offset,
				     iovec *iov, int iovcnt) {
    return new file_backend_req(OP_READ, name, iov, iovcnt, offset, ioctx);
}

request *file_backend::make_read_req(const char *name, size_t offset,
				     char *buf, size_t len) {
    iovec iov = {buf, len};
    return new file_backend_req(OP_READ, name, &iov, 1, offset, ioctx);
}
