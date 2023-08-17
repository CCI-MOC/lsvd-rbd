/*
 * file:        objname.h
 * description: string-like structure for handling object names
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef __OBJNAME_H__
#define __OBJNAME_H__

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

class objname
{
    char buf[128];

  public:
    objname() {}
    objname(const char *prefix, uint32_t seq) { init(prefix, seq); }
    void init(const char *prefix, uint32_t seq)
    {
        size_t len = strlen(prefix);
        assert(len + 9 < sizeof(buf));
        memcpy(buf, prefix, len);
        sprintf(buf + len, ".%08x", seq);
    }
    const char *c_str() { return buf; }
};

// On-disk representation for read-cache object mapping
struct objname_map {
    char obj[128];
    int offset;

  public:
    /* Required for unordered_map */
    bool operator==(const objname_map other) const
    {
        return (!strcmp(obj, other.obj)) && (offset == other.offset);
    }

    /* Helper function to initialize objname_map */
    static inline objname_map make_key(objname obj, int offset)
    {
        objname_map key = { {0}, offset };
        strncpy(key.obj, obj.c_str(), 128);
        return key;
    }
};

template <class T> inline void hash_combine(std::size_t &s, const T &v)
{
    std::hash<T> h;
    s ^= h(v) + 0x9e37779b9 + (s << 6) + (s >> 2);
}

namespace std
{
/*
 * Hash function for objname_map, so it can be used as a key for hash map
 * Required for unordered_map
 */
template <> struct hash<objname_map> {
    auto operator()(const objname_map &obj) const
    {
        size_t res = 0;
        string objstr(obj.obj); // hashing std::string is easier than char*
        hash_combine(res, objstr);
        hash_combine(res, obj.offset);
        return res;
    }
};
} // namespace std

#endif
