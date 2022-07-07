#include "lsvd_includes.h"
#include "base_functions.h"
#include "batch.h"

    void batch::reset(void) {
        len = 0;
        entries.resize(0);
        seq = batch_seq++;
    }

    void batch::append_iov(uint64_t lba, iovec *iov, int iovcnt) {
        char *ptr = buf + len;
        for (int i = 0; i < iovcnt; i++) {
            memcpy(ptr, iov[i].iov_base, iov[i].iov_len);
            entries.push_back((data_map){lba, iov[i].iov_len / 512});
            ptr += iov[i].iov_len;
            len += iov[i].iov_len;
            lba += iov[i].iov_len / 512;
        }
    }

    int batch::hdrlen(void) {
        return sizeof(hdr) + sizeof(data_hdr) + entries.size() * sizeof(data_map);
    }

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
    bool thread_pool<T>::get(T &val) {
        std::unique_lock<std::mutex> lk(*m);
        return get_locked(lk, val);
    }
    template<class T>
    bool thread_pool<T>::wait_locked(std::unique_lock<std::mutex> &lk) {
        while (running && q.empty())
            cv.wait(lk);
        return running;
    }
    template<class T>
    bool thread_pool<T>::get_nowait(T &val) {
        if (!running || q.empty())
            return false;
        val = q.front();
        q.pop();
        return true;
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

    static bool aligned(const void *ptr, int a)
    {
         return 0 == ((long)ptr & (a-1));
    }


