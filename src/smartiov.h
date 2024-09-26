#pragma once
#include <cassert>
#include <string.h>
#include <sys/uio.h>

#include "utils.h"

using byte = std::byte;
using usize = size_t;

struct buffer {
    byte *buf;
    usize len;
};

/* this makes readv / writev a lot easier...
 */
class smartiov
{
    vec<iovec> iovs;

  public:
    static smartiov from_buf(vec<byte> &buf)
    {
        return smartiov((char *)buf.data(), buf.size());
    }

    static smartiov from_str(std::string_view str)
    {
        return smartiov((char *)str.data(), str.size());
    }

    static smartiov from_iovecs(const iovec *iov, int iovcnt)
    {
        smartiov siov;
        for (int i = 0; i < iovcnt; i++)
            if (iov[i].iov_len > 0)
                siov.iovs.push_back(iov[i]);
        return siov;
    }

    static smartiov from_ptr(void *ptr, size_t len)
    {
        return smartiov((char *)ptr, len);
    }

    smartiov() {}

    smartiov(const iovec *iov, int iovcnt)
    {
        for (int i = 0; i < iovcnt; i++)
            if (iov[i].iov_len > 0)
                iovs.push_back(iov[i]);
    }

    smartiov(char *buf, size_t len) { iovs.push_back((iovec){buf, len}); }
    void push_back(const iovec &iov) { iovs.push_back(iov); }

    void ingest(const iovec *iov, int iovcnt)
    {
        for (int i = 0; i < iovcnt; i++)
            if (iov[i].iov_len > 0)
                iovs.push_back(iov[i]);
    }

    iovec *data(void) { return iovs.data(); }
    iovec &operator[](int i) { return iovs[i]; }
    int size(void) { return iovs.size(); }

    vec<iovec> &iovs_vec(void) { return iovs; }

    size_t bytes(void)
    {
        size_t sum = 0;
        for (auto i : iovs)
            sum += i.iov_len;
        return sum;
    }

    smartiov slice(size_t off, size_t limit)
    {
        assert(limit <= bytes());
        smartiov other;
        size_t len = limit - off;
        for (auto it = iovs.begin(); it != iovs.end() && len > 0; it++) {
            if (it->iov_len < off)
                off -= it->iov_len;
            else {
                auto _len = std::min(len, it->iov_len - off);
                other.push_back((iovec){(char *)it->iov_base + off, _len});
                len -= _len;
                off = 0;
            }
        }
        return other;
    }

    void zero(void)
    {
        for (auto i : iovs)
            memset(i.iov_base, 0, i.iov_len);
    }

    void zero(size_t off, size_t limit)
    {
        size_t len = limit - off;
        for (auto it = iovs.begin(); it != iovs.end() && len > 0; it++) {
            if (it->iov_len < off)
                off -= it->iov_len;
            else {
                auto _len = std::min(len, it->iov_len - off);
                memset((char *)it->iov_base + off, 0, _len);
                len -= _len;
                off = 0;
            }
        }
    }

    void copy_in(byte *buf, size_t limit)
    {
        if (limit == 0) // do nothing
            return;

        assert(buf != nullptr);
        auto remaining = limit;
        for (auto i : iovs) {
            auto to_copy = std::min(remaining, i.iov_len);
            remaining -= to_copy;

            memcpy((void *)i.iov_base, (void *)buf, to_copy);
            buf += i.iov_len;

            if (remaining == 0)
                break;
        }
    }

    void copy_out(byte *buf)
    {
        for (auto i : iovs) {
            memcpy((void *)buf, (void *)i.iov_base, (size_t)i.iov_len);
            buf += i.iov_len;
        }
    }
};
