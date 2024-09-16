#pragma once

#include <cstddef>
#include <cstdint>
#include <folly/FBString.h>
#include <unistd.h>

using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;
using u8 = uint8_t;
using s64 = int64_t;
using s32 = int32_t;
using s16 = int16_t;
using s8 = int8_t;
using usize = size_t;
using ssize = ssize_t;
using byte = std::byte;
using str = folly::fbstring;

using seqnum_t = uint32_t;

/**
Represents an extent on the backend. We allow a special object_id = offset = 0
to represent NULL ranges that are not currently mapped no anything

This just represents a range of bytes in an object; it does not know about
metadata, headers, or anything else about the contents of those ranges. Users
need to take care to ensure they know if this is pointing to ranges that
are actually relevant to what they might care about.
 */
struct S3Ext {
    seqnum_t seqnum;
    uint32_t offset;
    uint64_t len;
};

enum log_entry_type {
    WRITE = 0,
    TRIM = 1,
};

struct __attribute((packed)) log_write_entry {
    log_entry_type type : 2;
    uint64_t offset : 62;
    uint64_t len;
    std::byte data[];
};

const uint32_t LOG_WRITE_ENTRY_SIZE = sizeof(log_write_entry);

inline auto get_data_ext(S3Ext ext)
{
    return S3Ext{
        .seqnum = ext.seqnum,
        .offset = ext.offset + LOG_WRITE_ENTRY_SIZE,
        .len = ext.len - LOG_WRITE_ENTRY_SIZE,
    };
}

struct __attribute((packed)) log_trim_entry {
    log_entry_type type : 2;
    uint64_t offset : 62;
    uint64_t len;
};

const auto LOG_TRIM_ENTRY_SIZE = sizeof(log_trim_entry);
