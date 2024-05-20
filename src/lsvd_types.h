#pragma once

#include <fmt/format.h>
#include <stddef.h>
#include <stdint.h>
#include <vector>

using sector_t = int64_t;
using page_t = int32_t;
using seqnum_t = uint32_t;

enum lsvd_op { OP_READ = 2, OP_WRITE = 4 };

enum { LSVD_MAGIC = 0x4456534c };

/* if buf[offset]...buf[offset+len] contains an array of type T,
 * copy them into the provided output vector
 */
template <class T>
void decode_offset_len(char *buf, size_t offset, size_t len,
                       std::vector<T> &vals)
{
    T *p = (T *)(buf + offset), *end = (T *)(buf + offset + len);
    for (; p < end; p++)
        vals.push_back(*p);
}

/* buf[offset ... offset+len] contains array of type T, with variable
 * length field name_len.
 */
template <class T>
void decode_offset_len_ptr(char *buf, size_t offset, size_t len,
                           std::vector<T *> &vals)
{
    T *p = (T *)(buf + offset), *end = (T *)(buf + offset + len);
    for (; p < end;) {
        vals.push_back(p);
        p = (T *)((char *)p + sizeof(T) + p->name_len);
    }
}

static inline int div_round_up(int n, int m) { return (n + m - 1) / m; }

static inline int round_up(int n, int m) { return m * div_round_up(n, m); }

static inline int round_down(int n, int m) { return m * (n / m); }

static inline bool aligned(const void *ptr, int a)
{
    return 0 == ((long)ptr & (a - 1));
}

class objname
{
    std::string name;

  public:
    objname(std::string prefix, uint32_t seq)
    {
        name = fmt::format("{}.{:08x}", prefix, seq);
    }

    std::string str() { return name; }
    const char *c_str() { return name.c_str(); }
};

static inline std::string oname(std::string prefix, uint32_t seq)
{
    return fmt::format("{}.{:08x}", prefix, seq);
}
