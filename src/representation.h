#pragma once

#include <cstddef>
#include <cstdint>
#include <folly/AtomicHashMap.h>
#include <folly/FBString.h>
#include <folly/FBVector.h>
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
using fstr = folly::fbstring;
using strv = std::string_view;
using string = std::string;

using seqnum_t = uint32_t;
const u64 LSVD_MAGIC = 0x4c5356444c535644; // "LSVDLSVD"

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

// === Log headers ===

struct __attribute((packed)) log_obj_hdr {
    uint64_t magic;
    uint64_t total_bytes;
    seqnum_t seqnum;
    uint32_t num_entries;
    byte data[];
};

struct __attribute((packed)) superblock_hdr {
    uint64_t magic;
    uint64_t total_bytes;
    byte data[];
};

// === Log entries ===

enum log_entry_type {
    WRITE = 0,
    TRIM = 1,
};

struct __attribute((packed)) log_entry {
    log_entry_type type : 2;
    uint64_t offset : 62;
    uint64_t len;
    byte data[];
};

const uint32_t LOG_ENTRY_SIZE = sizeof(log_entry);
static_assert(LOG_ENTRY_SIZE == 16);

inline auto get_data_ext(S3Ext ext)
{
    return S3Ext{
        .seqnum = ext.seqnum,
        .offset = ext.offset + LOG_ENTRY_SIZE,
        .len = ext.len - LOG_ENTRY_SIZE,
    };
}

inline auto get_logobj_key(strv image_name, seqnum_t seqnum)
{
    return fmt::format("{}.{:08x}", image_name, seqnum);
}

inline auto get_cache_key(strv image_name, seqnum_t seqnum, usize offset)
{
    return fmt::format("{}.{:08x}:{}", image_name, seqnum, offset);
}

// === Journal entries ===

struct __attribute((packed)) journal_entry {
    log_entry_type type : 2;
    uint64_t offset : 62;
    uint64_t len;
    byte data[];
};

const uint32_t JOURNAL_ENTRY_SIZE = sizeof(journal_entry);
static_assert(JOURNAL_ENTRY_SIZE == 16);
