// file:	misc_cache.h
// description: contains several of the important classes and structures used directly by 
//		translate and cache classes. As the name suggests, contains mostly miscellaneous
//		functions and class/structure definitions:
//		        -thread_pool class for translate class (also utilized by caches)
//		        -cache_work structure for caches
//		        -sized_vector for caches
//		        -commonly used objmap structure modified with mutex from extent.h
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

#ifndef MISC_CACHE_H
#define MISC_CACHE_H

/* implements a thread pool with a work queue of type T
 * to use, push threads onto thread_pool.pool
 * - get(), get_locked() - used by worker threads to receive work
 * - put(), put_locked() - submit work
 */
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

    void stop() {
	std::unique_lock lk(*m);
	running = false;
	cv.notify_all();
	lk.unlock();
	while (!pool.empty()) {
	    pool.front().join();
	    pool.pop();
	}
    }
    ~thread_pool() {
        stop();
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

    void put_locked(T work) {
        q.push(work);
        cv.notify_one();
    }

    void put(T work) {
        std::unique_lock<std::mutex> lk(*m);
        put_locked(work);
    }
};

/* if buf[offset]...buf[offset+len] contains an array of type T,
 * copy them into the provided output vector
 */
template<class T>
void decode_offset_len(char *buf, size_t offset, size_t len,
                       std::vector<T> &vals) {
    T *p = (T*)(buf + offset), *end = (T*)(buf + offset + len);
    for (; p < end; p++)
        vals.push_back(*p);
}


// objmap:	a modified object map whose only difference is the containment of a mutex to
//		be used with the extmap::objmap used inside it. For documentation on extmap
//		see extent.h
class objmap {
public:
    std::shared_mutex m;
    extmap::objmap    map;
};

/* nice error messages
 */
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
static inline void throw_fs_error(std::string msg) {
    throw fs::filesystem_error(msg, std::error_code(errno,
                                                    std::system_category()));
}

// cache_work:	This is a structure for support in using the read_cache and write_cache objects
//		It contains callback to function, sectors for the caches, smartiov (see smartiov.h
//		for documentation), and a constructure for itself
struct cache_work {
public:
    uint64_t  lba;
    void    (*callback)(void*);
    void     *ptr;
    sector_t     sectors;
    smartiov  iovs;
    cache_work(sector_t _lba, const iovec *iov, int iovcnt,
	       void (*_callback)(void*), void *_ptr) : iovs(iov, iovcnt) {
	lba = _lba;
	sectors = iovs.bytes() / 512;
	callback = _callback;
	ptr = _ptr;
    }
};


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