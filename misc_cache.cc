#include <shared_mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include "base_functions.h"
#include "smartiov.h"
#include "extent.h"
#include <mutex>

#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;

#include "misc_cache.h"

    template<class T>
    bool thread_pool<T>::get_locked(std::unique_lock<std::mutex> &lk, T &val) {
        while (running && q.empty())
            cv.wait(lk);
        if (!running)
            return false;
        val = q.front();
        q.pop();
        return val;
    }
    template<class T>
    void thread_pool<T>::put_locked(T work) {
        q.push(work);
        cv.notify_one();
    }
    template<class T>
    void thread_pool<T>::put(T work) {
        std::unique_lock<std::mutex> lk(*m);
        put_locked(work);
    }

    template<class T>
    void decode_offset_len(char *buf, size_t offset, size_t len, std::vector<T> &vals) {
        T *p = (T*)(buf + offset), *end = (T*)(buf + offset + len);
        for (; p < end; p++)
            vals.push_back(*p);
    }

    void throw_fs_error(std::string msg) {
        throw fs::filesystem_error(msg, std::error_code(errno, std::system_category()));
    }
