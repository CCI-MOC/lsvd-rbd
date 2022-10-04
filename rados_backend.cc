/*
 * file:        rados_backend.cc
 * description: backend interface using RADOS objects
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <rados/librados.h>

#include "smartiov.h"
#include "extent.h"
#include "backend.h"

#include <vector>
#include <sstream>
#include <iomanip>

#include <algorithm>		// needed for wrap()
#include "base_functions.h"

#include <condition_variable>
#include <queue>
#include <thread>

#include "rados_backend.h"

class rados_backend : public backend {
    std::mutex m;
    char *pool = NULL;
    char *prefix = NULL;
    rados_t cluster;
    rados_ioctx_t io_ctx;

    void pool_create(char *pool_);
    
public:
    rados_backend();
    ~rados_backend();

    int write_object(const char *name, iovec *iov, int iovcnt);
    int read_object(const char *name, iovec *iov, int iovcnt,
                    size_t offset);
    int delete_object(const char *name);
    
    /* async I/O
     */
    request *make_write_req(const char *name, iovec *iov, int iovcnt);
    request *make_read_req(const char *name, size_t offset,
                           iovec *iov, int iovcnt);
    request *make_read_req(const char *name, size_t offset,
                           char *buf, size_t len);
};    

/* needed for implementation hiding
 */
backend *make_rados_backend() {
    return new rados_backend;
}

/* see https://docs.ceph.com/en/latest/rados/api/librados/
 * for documentation on the librados API
 */
rados_backend::rados_backend() {
    int r;
    if ((r = rados_create(&cluster, NULL)) < 0) // NULL = ".client"
	throw("rados create");
    if ((r = rados_conf_read_file(cluster, NULL)) < 0)
	throw("rados conf");
    if ((r = rados_connect(cluster)) < 0)
	throw("rados connect");
}

rados_backend::~rados_backend() {
    /* TODO: do we close things down? */
}

/* TODO: make rados_backend take an io_ctx, dispense with all this
 * pool nonsense. Means all images go in the same pool
 */
void rados_backend::pool_create(char *pool_) {
    if (pool != NULL)
	assert(!strcmp(pool, pool_));
    else {
	pool = strdup(pool_);
	int r = rados_ioctx_create(cluster, pool, &io_ctx);
	if (r < 0)
	    throw("rados pool open");
    }
}

/* really gross, just special case of strtok. 
 */
std::pair<char*,char*> split_name(char *name) {
    char *p = strchr(name, '/');
    *p = 0;
    return std::pair(name, p+1);
}

int rados_backend::write_object(const char *name, iovec *iov, int iovcnt) {
    char name_buf[128];
    strcpy(name_buf, name);
    auto [pool,oname] = split_name(name_buf);
    pool_create(pool);
    
    smartiov iovs(iov, iovcnt);
    char *buf = (char*)malloc(iovs.bytes());
    iovs.copy_out(buf);

    int r = rados_write(io_ctx, oname, buf, iovs.bytes(), 0);

    free(buf);
    return r;
}

int rados_backend::read_object(const char *name, iovec *iov,
				   int iovcnt, size_t offset) {

    char name_buf[128];
    strcpy(name_buf, name);
    auto [pool,oname] = split_name(name_buf);
    pool_create(pool);
    
    smartiov iovs(iov, iovcnt);
    char *buf = (char*)malloc(iovs.bytes());

    int r = rados_read(io_ctx, oname, buf, iovs.bytes(), offset);

    iovs.copy_in(buf);
    free(buf);
    return r;
}

int rados_backend::delete_object(const char *name) {
    char name_buf[128];
    strcpy(name_buf, name);
    auto [pool,oname] = split_name(name_buf);
    pool_create(pool);
    return rados_remove(io_ctx, oname);
}

class rados_be_request : public request {
    smartiov       _iovs;
    char          *buf = NULL;
    request       *parent = NULL;
    char          *oid = NULL;
    size_t         offset = 0;
    rados_ioctx_t  io_ctx;
    rados_completion_t c;

public:
    enum lsvd_op   op;

    rados_be_request(enum lsvd_op op_, char *obj_name,
		     iovec *iov, int niov, size_t offset_,
		     rados_ioctx_t io_ctx_) : _iovs(iov, niov) {
	offset = offset_;
	io_ctx = io_ctx_;
	oid = obj_name;
	op = op_;
    }
    ~rados_be_request() {}

    void notify(request *unused) {
	if (op == OP_READ && buf != NULL)
	    _iovs.copy_in(buf);
	parent->notify(this);
	if (buf != NULL)
	    free(buf);
	delete this;
    }

    static void rados_be_notify(rados_completion_t c, void *ptr) {
	auto req = (rados_be_request*)ptr;
	req->notify(NULL);
    }
    
    void run(request *parent_) {
	parent = parent_;
	assert(op == OP_READ || op == OP_WRITE);
	
	/* damn C interface doesn't handle iovecs
	 */
	char *_buf = (char*)_iovs.data()->iov_base;
	if (_iovs.size() > 1) {
	    _buf = buf = (char*)malloc(_iovs.bytes());
	    if (op == OP_WRITE)
		_iovs.copy_out(buf);
	}

	rados_aio_create_completion(this, rados_be_notify, NULL, &c);
	if (op == OP_READ)
	    rados_aio_read(io_ctx, oid, c, _buf, _iovs.bytes(), offset);
	else
	    rados_aio_write(io_ctx, oid, c, _buf, _iovs.bytes(), offset);
    }

    sector_t lba() { return 0;}
    smartiov *iovs() { return NULL; }
    bool is_done() { return false; }
    void release() {}
    void wait() {}
};

request *rados_backend::make_write_req(const char *name, iovec *iov,
				       int iovcnt) {
    char name_buf[128];
    strcpy(name_buf, name);
    auto [pool,oid] = split_name(name_buf);
    pool_create(pool);
    return new rados_be_request(OP_WRITE, oid, iov, iovcnt, 0, io_ctx);
}

request *rados_backend::make_read_req(const char *name, size_t offset,
				      iovec *iov, int iovcnt) {
    char name_buf[128];
    strcpy(name_buf, name);
    auto [pool,oid] = split_name(name_buf);
    pool_create(pool);
    return new rados_be_request(OP_READ, oid, iov, iovcnt, offset, io_ctx);
}

request *rados_backend::make_read_req(const char *name, size_t offset,
				      char *buf, size_t len) {
    iovec iov = {buf, len};
    char name_buf[128];
    strcpy(buf, name);
    auto [pool,oid] = split_name(name_buf);
    pool_create(pool);
    return new rados_be_request(OP_READ, oid, &iov, 1, offset, io_ctx);
}
