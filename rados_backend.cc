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

#include "rados_backend.h"

    std::pair<std::string,std::string> split_string(std::string s, std::string delim)
    {
        auto i = s.find(delim);
        return std::pair(s.substr(0,i), s.substr(i+delim.length()));
    }

    ssize_t rados_backend::write_object(const char *name, iovec *iov, int iovcnt) {
        smartiov iovs(iov, iovcnt);
        char *buf = (char*)malloc(iovs.bytes());
        iovs.copy_out(buf);
        int r = rados_write(io_ctx, name, buf, iovs.bytes(), 0);
        free(buf);
        return r;
    }
    ssize_t rados_backend::write_numbered_object(int seq, iovec *iov, int iovcnt) {
        char name[128];
        sprintf(name, "%s.%08x", prefix, seq);
        //auto name = std::string(prefix) + "." + hex(seq);
        return write_object(name, iov, iovcnt);
    }
    void rados_backend::delete_numbered_object(int seq) {
        char name[128];
        sprintf(name, "%s.%08x", prefix, seq);
        rados_remove(io_ctx, name);
    }
    ssize_t rados_backend::read_object(const char *name, char *buf, size_t len, size_t offset) {
        return rados_read(io_ctx, name, buf, len, offset);
    }
    ssize_t rados_backend::read_numbered_object(int seq, char *buf, size_t len, size_t offset) {
        //auto name = std::string(prefix) + "." + hex(seq);
        char name[128];
        sprintf(name, "%s.%08x", prefix, seq);
        //printf("p: %s +: %s\n", name2, name.c_str());
        return read_object(name, buf, len, offset);
    }
    ssize_t rados_backend::read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset) {
        smartiov iovs(iov, iovcnt);
        char *buf = (char*)malloc(iovs.bytes());
        int r = read_numbered_object(seq, buf, iovs.bytes(), offset);
        iovs.copy_in(buf);
        free(buf);
        return r;
    }

    void rados_backend::aio_read_done(rados_completion_t c, void *ptr) {
        auto aio = (rados_aio*)ptr;
        aio->cb(aio->ptr);
        rados_aio_release(aio->c);
        delete aio;
    }
    int rados_backend::aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
                        void (*cb)(void*), void *ptr)
    {
        auto name = std::string(prefix) + "." + hex(seq);
        rados_aio *aio = new rados_aio;
        aio->cb = cb;
        aio->ptr = ptr;
        assert(buf != NULL);
        rados_aio_create_completion((void*)aio, aio_read_done, NULL, &aio->c);
        return rados_aio_read(io_ctx, name.c_str(), aio->c, buf, len, offset);
    }
    int rados_backend::aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
                                  void (*cb)(void*), void *ptr) {
        auto name = std::string(prefix) + "." + hex(seq);
        auto rv = write_object(name.c_str(), iov, iovcnt);
        cb(ptr);
        return rv;
    }

    std::string rados_backend::object_name(int seq) {
        return std::string(prefix) + "." + hex(seq);
    }



