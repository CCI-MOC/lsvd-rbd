#ifndef BATCH_H
#define BATCH_H

#include "lsvd_includes.h"
#include "base_functions.h"

class backend {
public:
    virtual ssize_t write_object(const char *name, iovec *iov, int iovcnt) = 0;
    virtual ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) = 0;
    virtual ssize_t read_object(const char *name, char *buf, size_t len, size_t offset) = 0;
    virtual ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt,
					  size_t offset) = 0;
    virtual void    delete_numbered_object(int seq) = 0;
    virtual ssize_t read_numbered_object(int seq, char *buf, size_t len,
					 size_t offset) = 0;
    virtual int aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
				    void (*cb)(void*), void *ptr) = 0;
    virtual int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
					  void (*cb)(void*), void *ptr) = 0;
    virtual std::string object_name(int seq) = 0;
    virtual ~backend(){}
};

struct batch {
    char  *buf;
    size_t max;
    size_t len;
    int    seq;
    std::vector<data_map> entries;
public:
    batch(size_t _max) {
	buf = (char*)malloc(_max);
	max = _max;
    }
    ~batch(){
	free((void*)buf);
    }
    void reset(void) {
	len = 0;
	entries.resize(0);
	seq = batch_seq++;
    }
    void append_iov(uint64_t lba, iovec *iov, int iovcnt) {
	char *ptr = buf + len;
	for (int i = 0; i < iovcnt; i++) {
	    memcpy(ptr, iov[i].iov_base, iov[i].iov_len);
	    entries.push_back((data_map){lba, iov[i].iov_len / 512});
	    ptr += iov[i].iov_len;
	    len += iov[i].iov_len;
	    lba += iov[i].iov_len / 512;
	}
    }
    int hdrlen(void) {
	return sizeof(hdr) + sizeof(data_hdr) + entries.size() * sizeof(data_map);
    }
};

template <class T>
class thread_pool {
public:
    std::queue<T> q;
    bool         running;
    std::mutex  *m;
    std::condition_variable cv;
    std::queue<std::thread> pool;
    
    thread_pool(std::mutex *_m) {
	running = true;
	m = _m;
    }
    ~thread_pool() {
	std::unique_lock lk(*m);
	running = false;
	cv.notify_all();
	lk.unlock();
	while (!pool.empty()) {
	    pool.front().join();
	    pool.pop();
	}
    }	
    bool get_locked(std::unique_lock<std::mutex> &lk, T &val) {
	while (running && q.empty())
	    cv.wait(lk);
	if (!running)
	    return false;
	val = q.front();
	q.pop();
	return val;
    }
    bool get(T &val) {
	std::unique_lock<std::mutex> lk(*m);
	return get_locked(lk, val);
    }
    bool wait_locked(std::unique_lock<std::mutex> &lk) {
	while (running && q.empty())
	    cv.wait(lk);
	return running;
    }
    bool get_nowait(T &val) {
	if (!running || q.empty())
	    return false;
	val = q.front();
	q.pop();
	return true;
    }
    void put_locked(T work) {
	q.push(work);
	cv.notify_one();
    }
    void put(T work) {
	std::unique_lock<std::mutex> lk(*m);
	put_locked(work);
    }
};

/* these all should probably be combined with the stuff in objects.cc to create
 * object classes that serialize and de-serialize themselves. Sometime, maybe.
 */
template <class T>
void decode_offset_len(char *buf, size_t offset, size_t len, std::vector<T> &vals) {
    T *p = (T*)(buf + offset), *end = (T*)(buf + offset + len);
    for (; p < end; p++)
	vals.push_back(*p);
}

class objmap {
public:
    std::shared_mutex m;
    extmap::objmap    map;
};

void throw_fs_error(std::string msg) {
    throw fs::filesystem_error(msg, std::error_code(errno, std::system_category()));
}

#endif
