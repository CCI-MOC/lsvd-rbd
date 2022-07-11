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
    ~batch() {
        free((void*)buf);
    }

    void reset(void);
    void append_iov(uint64_t lba, iovec *iov, int iovcnt);
    int hdrlen(void);
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
    bool get_locked(std::unique_lock<std::mutex> &lk, T &val);
    bool get(T &val);
    bool wait_locked(std::unique_lock<std::mutex> &lk);
    bool get_nowait(T &val);
    void put_locked(T work);
    void put(T work);
};

/* these all should probably be combined with the stuff in objects.cc to create
 * object classes that serialize and de-serialize themselves. Sometime, maybe.
 */
template<class T>
void decode_offset_len(char *buf, size_t offset, size_t len, std::vector<T> &vals);


class objmap {
public:
    std::shared_mutex m;
    extmap::objmap    map;
};

void throw_fs_error(std::string msg);

struct cache_work {
public:
    uint64_t  lba;
    void    (*callback)(void*);
    void     *ptr;
    lba_t     sectors;
    smartiov  iovs;
    cache_work(lba_t _lba, const iovec *iov, int iovcnt,
	       void (*_callback)(void*), void *_ptr) : iovs(iov, iovcnt) {
	lba = _lba;
	sectors = iovs.bytes() / 512;
	callback = _callback;
	ptr = _ptr;
    }
};

/* misc helpers stuff */

// static bool aligned(const void *ptr, int a);

//static int round_up(int n, int m);

/* convenience class, because we don't know cache size etc.
 * at cache object construction time.
 */
template <class T>
class sized_vector {
    std::vector<T> *elements;
public:
    ~sized_vector() {
        delete elements;
    }
    void init(int n) {
        elements = new std::vector<T>(n);
    }
    void init(int n, T val) {
        elements = new std::vector<T>(n, val);
    }
    T &operator[](int index) {
        return (*elements)[index];
    }
};

#endif
