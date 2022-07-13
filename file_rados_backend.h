#ifndef FILE_RADOS_BACKEND_H
#define FILE_RADOS_BACKEND_H

#include <rados/librados.h>
#include <map>

#include "backend.h"
#include "base_functions.h"
#include "io.h"
#include "misc_cache.h"

class file_backend : public backend {
    char *prefix;
    std::mutex m;
    std::map<int,int> cached_fds;
    std::queue<int>   cached_nums;
    static const int  fd_cache_size = 500;

    int get_cached_fd(int seq);

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
    ssize_t write_object(const char *name, iovec *iov, int iovcnt);
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt);
    void delete_numbered_object(int seq);
    ssize_t read_object(const char *name, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_object(int seq, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset);
    int aio_read_num_object(int seq, char *buf, size_t len,
			    size_t offset, void (*cb)(void*), void *ptr);
    int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
				  void (*cb)(void*), void *ptr);
    ~file_backend() {
	free((void*)prefix);
	for (auto it = cached_fds.begin(); it != cached_fds.end(); it++)
	    close(it->second);

	e_io_running = false;
	e_io_th.join();
	io_queue_release(ioctx);
    }
    std::string object_name(int seq);
};

std::pair<std::string,std::string> split_string(std::string s, std::string delim);


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
    ssize_t write_object(const char *name, iovec *iov, int iovcnt);
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt);
    void delete_numbered_object(int seq);
    ssize_t read_object(const char *name, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_object(int seq, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset);

    struct rados_aio {
	void (*cb)(void*);
	void *ptr;
	rados_completion_t c;
    };

    static void aio_read_done(rados_completion_t c, void *ptr);
    int aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
			void (*cb)(void*), void *ptr);
    int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
				  void (*cb)(void*), void *ptr);

    ~rados_backend() {
	free((void*)prefix);
	rados_ioctx_destroy(io_ctx);
	rados_shutdown(cluster);
    }
    std::string object_name(int seq);
};

#endif
