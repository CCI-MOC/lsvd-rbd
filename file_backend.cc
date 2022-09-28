#include <libaio.h>
#include <rados/librados.h>
#include <map>
#include <queue>
#include <condition_variable>
#include <shared_mutex>
#include <thread>

#include "smartiov.h"
#include "extent.h"
#include "backend.h"

#include <uuid/uuid.h>
#include <sys/uio.h>

#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>

#include "base_functions.h"
#include "io.h"
#include "misc_cache.h"

#include <fcntl.h>
#include "file_backend.h"

file_backend::file_backend(const char *_prefix) {
    prefix = strdup(_prefix);
    e_io_running = true;
    io_queue_init(64, &ioctx);
    const char *name = "file_backend_cb";
    e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
}


ssize_t file_backend::write_object(const char *name, iovec *iov, int iovcnt) {
    int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0777);
    if (fd < 0)
	return -1;
    auto val = writev(fd, iov, iovcnt);
    close(fd);
    return val;
}

ssize_t file_backend::write_numbered_object(int seq, iovec *iov, int iovcnt) {
    auto name = std::string(prefix) + "." + hex(seq);
    return write_object(name.c_str(), iov, iovcnt);
}

void file_backend::delete_numbered_object(int seq) {
    auto name = std::string(prefix) + "." + hex(seq);
    unlink(name.c_str());
}

ssize_t file_backend::read_object(const char *name, char *buf, size_t len,
				  size_t offset) {
    int fd = open(name, O_RDONLY);
    if (fd < 0)
	return -1;
    auto val = pread(fd, buf, len, offset);
    close(fd);
    return val;
}
ssize_t file_backend::read_numbered_object(int seq, char *buf, size_t len,
					   size_t offset) {
    iovec iov = {buf, len};
    return read_numbered_objectv(seq, &iov, 1, offset);
}
    
ssize_t file_backend::read_numbered_objectv(int seq, iovec *iov, int iovcnt,
					    size_t offset) {
    auto name = std::string(prefix) + "." + hex(seq);
    auto fd = open(name.c_str(), O_RDONLY);
    auto val = preadv(fd, iov, iovcnt, offset);
    close(fd);
    return val;
}

struct rw_cb_data {
    void (*cb)(void*);
    void *ptr;
    int fd;
    rw_cb_data(void (*cb_)(void*), void* ptr_, int fd_) :
	cb(cb_), ptr(ptr_), fd(fd_) {};
};
void rw_cb_fn(void *ptr) {
    auto data = (rw_cb_data*)ptr;
    close(data->fd);
    data->cb(data->ptr);
    delete data;
}
    
int file_backend::aio_read_num_object(int seq, char *buf, size_t len,
				      size_t offset,
				      void (*cb)(void*), void *ptr) {
    auto name = std::string(prefix) + "." + hex(seq);
    auto fd = open(name.c_str(), O_RDONLY);
    if (fd < 0)
	return -1;
    auto data = new rw_cb_data(cb, ptr, fd);
    auto eio = new e_iocb;
    
    e_io_prep_pread(eio, fd, buf, len, offset, rw_cb_fn, (void*)data);
    e_io_submit(ioctx, eio);
    return 0;
}
    
int file_backend::aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
					    void (*cb)(void*), void *ptr) {
    auto name = std::string(prefix) + "." + hex(seq);
    int fd = open(name.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0777);
    if (fd < 0)
	return -1;
    auto data = new rw_cb_data(cb, ptr, fd);
    auto eio = new e_iocb;
    size_t offset = 0;
    e_io_prep_pwritev(eio, fd, iov, iovcnt, offset, rw_cb_fn, (void*)data);
    e_io_submit(ioctx, eio);
    return 0;
}

file_backend::~file_backend() {
    free((void*)prefix);

    e_io_running = false;
    e_io_th.join();
    io_queue_release(ioctx);
}

std::string file_backend::object_name(int seq) {
    return std::string(prefix) + "." + hex(seq);
}


