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
    usize total_bytes;
    vec<iovec> iovs;

  private:
    smartiov(char *buf, usize len) : total_bytes(len)
    {
        assert(len > 0);
        iovs.push_back((iovec){buf, len});
    }

    smartiov(vec<iovec> &&iovs) : iovs(std::move(iovs))
    {
        assert(this->iovs.size() > 0);
        for (auto [base, len] : this->iovs) {
            assert(len > 0);
            total_bytes += len;
        }
    }

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
        assert(iovcnt > 0);
        vec<iovec> iovs;
        for (int i = 0; i < iovcnt; i++)
            if (iov[i].iov_len > 0)
                iovs.push_back(iov[i]);
        return smartiov(std::move(iovs));
    }

    static smartiov from_ptr(void *ptr, usize len)
    {
        return smartiov((char *)ptr, len);
    }

    auto num_vecs(void) { return iovs.size(); }
    auto bytes(void) { return total_bytes; }
    const vec<iovec> &iovs_vec(void) { return iovs; }

    // end is exclusive
    smartiov slice(usize start, usize end)
    {
        assert(end <= total_bytes);
        vec<iovec> other;
        usize len = end - start;
        for (auto it = iovs.begin(); it != iovs.end() && len > 0; it++) {
            if (it->iov_len < start)
                start -= it->iov_len;
            else {
                auto _len = std::min(len, it->iov_len - start);
                other.push_back((iovec){(char *)it->iov_base + start, _len});
                len -= _len;
                start = 0;
            }
        }
        return smartiov(std::move(other));
    }

    void zero(void)
    {
        for (auto i : iovs)
            memset(i.iov_base, 0, i.iov_len);
    }

    void zero(usize off, usize limit)
    {
        usize len = limit - off;
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

    void copy_in(byte *buf, usize limit)
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
            memcpy((void *)buf, (void *)i.iov_base, (usize)i.iov_len);
            buf += i.iov_len;
        }
    }
};
