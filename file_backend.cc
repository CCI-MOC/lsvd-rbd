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
#include <errno.h>
#include <sys/stat.h>

#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <random>
#include <algorithm>

#include "lsvd_types.h"
#include "backend.h"
#include "request.h"
#include "smartiov.h"
#include "io.h"
#include "misc_cache.h"
#include "objects.h"

extern void do_log(const char *, ...);

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
    std::map<std::string,int> fds;
    std::vector<std::string> lru;
    const size_t max_fds = 32;
    
public:
    file_backend(const char *prefix);
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
    thread_pool<file_backend_req*> workers;

    int get_fd(const char* f);
};

/* try to prevent weird stuff happening when you open and close
 * lots of files on different threads.
 */
int file_backend::get_fd(const char* f) {
    std::unique_lock lk(m);
    int fd = -1;
    auto s = std::string(f);
    if (fds.find(s) == fds.end()) {
	if (lru.size() >= max_fds) {
	    auto evictee = lru.front();
	    auto it = fds.find(evictee);
	    assert(it != fds.end());
	    close(it->second);
	    fds.erase(it);
	    lru.erase(lru.begin());
	}
	assert((fd = open(f, O_RDONLY)) >= 0);
	char buf[512];
	assert(pread(fd, buf, sizeof(buf), 0) == sizeof(buf));
	auto hdr = (obj_hdr*)buf;
	uint32_t seq = strtol(f + strlen(f) - 8, NULL, 16);
	assert(seq == hdr->seq);
	lru.push_back(s);
	fds[s] = fd;
    }
    else {
	fd = fds[s];
	auto it = std::find(lru.begin(), lru.end(), s);
	lru.erase(it);
	lru.push_back(s);
    }

    return fd;
}

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

void trim_partial(const char *_prefix) {
    auto prefix = std::string(_prefix);
    auto stem = fs::path(prefix).filename();
    auto parent = fs::path(prefix).parent_path();
    size_t stem_len = strlen(stem.c_str());

    for (auto const& dir_entry : fs::directory_iterator{parent}) {
	std::string entry{dir_entry.path().filename()};
	if (strncmp(entry.c_str(), stem.c_str(), stem_len) == 0 &&
	    entry.size() == stem_len + 9) {
	    char buf[512];
	    auto h = (obj_hdr *)buf;
	    int fd = open(dir_entry.path().c_str(), O_RDONLY);
	    struct stat sb;
	    assert(fstat(fd, &sb) >= 0);
	    int rv = read(fd, buf, sizeof(buf));
	    if (rv < 512 || h->magic != LSVD_MAGIC ||
		(h->hdr_sectors + h->data_sectors)*512 != sb.st_size) {
		printf("deleting partial object: %s (%d vs %d)\n",
		       dir_entry.path().c_str(), (int)sb.st_size,
		       (int)(h->hdr_sectors + h->data_sectors));
		rename(dir_entry.path().c_str(),
		       (std::string(dir_entry.path()) + ".bak").c_str());
	    }
	    close(fd);
	}
    }
}

int iov_sum(iovec *iov, int niov) {
    int sum = 0;
    for (int i = 0; i < niov; i++)
	sum += iov[i].iov_len;
    return sum;
}

class file_backend_req : public request {
    enum lsvd_op    op;
    smartiov        _iovs;
    size_t          offset;
    char            name[64];
    request        *parent = NULL;
    file_backend   *be;
    int             fd = -1;
    
public:
    file_backend_req(enum lsvd_op op_, const char *name_,
		     iovec *iov, int iovcnt, size_t offset_,
		     file_backend *be_) : _iovs(iov, iovcnt) {
	op = op_;
	offset = offset_;
	be = be_;
	strcpy(name, name_);
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
	auto [iov,niovs] = _iovs.c_iov();
	if (op == OP_READ) {
	    fd = be->get_fd(name);
	    auto rv = preadv(fd, iov, niovs, offset);
	    assert(rv > 0);
	}
	else {
	    if ((fd = open(name,
			   O_RDWR | O_CREAT | O_TRUNC, 0777)) < 0)
		throw("file object error");
	    if (pwritev(fd, iov, niovs, offset) < 0)
		throw("file object error");
	    close(fd);
	}
	notify(NULL);
    }
};


/* methods that depend on file_backend_req
 */
file_backend::file_backend(const char *prefix) : workers(&m) {
    if (prefix)
	trim_partial(prefix);
    rng.seed(__lsvd_dbg_be_seed);
    for (int i = 0; i < __lsvd_dbg_be_threads; i++) 
	workers.pool.push(std::thread(&file_backend::worker, this, &workers));
}

file_backend::~file_backend() {
    for (auto [s,fd] : fds)
	close(fd);
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

/* TODO: run() ought to return error/success
 */
void file_backend_req::run(request *parent_) {
    parent = parent_;
    be->workers.put(this);
}

/* TODO: this assumes no use of wait/release
 */
void file_backend_req::notify(request *unused) {
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

backend *make_file_backend(const char *prefix) {
    return new file_backend(prefix);
}
