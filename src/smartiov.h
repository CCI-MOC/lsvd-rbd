#pragma once
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
    const usize total_bytes;
    const vec<iovec> iovs;

  private:
    smartiov(vec<iovec> &&v, usize len) : total_bytes(len), iovs(std::move(v))
    {
        ENSURE(iovs.size() > 0);
        usize total = 0;
        for (auto [base, len] : iovs)
            total += len;
        ENSURE(total == total_bytes);
    }

  public:
    static smartiov from_buf(vec<byte> &buf)
    {
        return smartiov({(iovec){buf.data(), buf.size()}}, buf.size());
    }

    static smartiov from_str(std::string_view str)
    {
        return smartiov({(iovec){(void *)str.data(), str.size()}}, str.size());
    }

    static smartiov from_ptr(void *ptr, usize len)
    {
        return smartiov({(iovec){ptr, len}}, len);
    }

    static smartiov from_iovecs(const iovec *iov, int iovcnt)
    {
        ENSURE(iovcnt > 0);
        usize total_len = 0;
        vec<iovec> iovs;
        for (int i = 0; i < iovcnt; i++)
            if (iov[i].iov_len > 0) {
                iovs.push_back(iov[i]);
                total_len += iov[i].iov_len;
            }

        return smartiov(std::move(iovs), total_len);
    }

    static smartiov from_iovecs(const iovec iov1)
    {
        ENSURE(iov1.iov_len > 0);
        return smartiov({iov1}, iov1.iov_len);
    }

    static smartiov from_iovecs(const iovec iov1, const iovec iov2)
    {
        ENSURE(iov1.iov_len > 0 || iov2.iov_len > 0);
        usize total_len = iov1.iov_len + iov2.iov_len;
        return smartiov({iov1, iov2}, total_len);
    }

    auto num_vecs(void) { return iovs.size(); }
    auto bytes(void) { return total_bytes; }
    const vec<iovec> &iovs_vec(void) { return iovs; }

    smartiov slice(usize start, usize len)
    {
        ENSURE(start + len <= total_bytes);
        vec<iovec> other;

        usize skip = start, remaining = len;
        for (auto [iovbase, iovlen] : iovs) {
            if (iovlen <= skip) {
                skip -= iovlen;
                continue;
            }

            auto use_len = std::min(remaining, iovlen - skip);
            ENSURE(use_len > 0);
            other.push_back((iovec){(byte *)iovbase + skip, use_len});
            remaining -= use_len;
            skip = 0;

            if (remaining == 0)
                break;
        }

        return smartiov(std::move(other), len);
    }

    void zero(usize start, usize len)
    {
        ENSURE(start + len <= total_bytes);

        usize skip = start, remaining = len;
        for (auto [iovbase, iovlen] : iovs) {
            if (iovlen <= skip) {
                skip -= iovlen;
                continue;
            }

            auto use_len = std::min(remaining, iovlen - skip);
            ENSURE(use_len > 0);
            memset((byte *)iovbase + skip, 0, use_len);
            remaining -= use_len;
            skip = 0;

            if (remaining == 0)
                break;
        }
    }

    void copy_in(byte *buf, usize len)
    {
        if (len == 0) // do nothing
            return;
        ENSURE(buf != nullptr);
        ENSURE(len <= total_bytes);

        auto remaining = len;
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
