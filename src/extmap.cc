#include <cassert>
#include <cstdint>
#include <folly/logging/xlog.h>
#include <memory>
#include <utility>

#include "extmap.h"
#include "folly/String.h"
#include "representation.h"
#include "zpp_bits.h"

Task<vec<std::pair<usize, S3Ext>>> ExtMap::lookup(usize offset, usize len)
{
    ENSURE(offset + len <= total_len);
    auto l = co_await mtx.co_scoped_lock_shared();
    vec<std::pair<usize, S3Ext>> res;

    // upper_bound returns the first element that is > offset, so the element
    // at which we start is always the one before that
    auto it = map.upper_bound(offset);

    // Go back one so we start at the right place
    it--;

    /*
    Terminology:
        ...-----|--- it = extent ---|---...
            |<------- len ----------------->|
            ^-- offset
            | bytes_found | bytes_remaining |
                          ^-- cur_off
                ^-- ext_start
                |<---- ext.len ---->|

    There are two possible cases:

    1. The base of the extent is *before* where we start reading
        |--- it = existing extent ----|---...
            a. |--- eq size read ----|
               |--- small read ---|
         or b. |--- large read ---------|

        This is only possible on the 1st iteration since all following
        iterations should have its ext_start align with bytes_remaining

    2. The base of the extent is the same as where we start reading
            ...---|--- it = existing extent ---|---...
            a. |--- small read ---|
               |--- same size read----------|
            b. |--- large read-------------------|
    */

    usize bytes_found = 0;
    while (bytes_found < len) {
        auto &[ext_start, ext] = *it;
        auto cur_off = offset + bytes_found;
        auto bytes_remaining = len - bytes_found;

        if (it == map.end() || ext_start > offset + len) {
            XLOGF(ERR, "Corrupt map detected on {}+{}", offset, len);
            XLOGF(DBG3, "Map: {}", co_await to_string());
            co_return {{offset, S3Ext{0, 0, len}}};
        }

        if (ext_start < cur_off) { // case 1
            // bytes_remaining is a. (<= ext.len) or b. (> ext.len)
            auto adjust = cur_off - ext_start;
            if (adjust >= UINT32_MAX && ext.seqnum != 0) {
                XLOGF(ERR,
                      "Corrupt extmap on read {}+{}. Already found {}, "
                      "current extent: start={},seq={},off={},len={} has "
                      "adjust {}",
                      offset, len, bytes_found, ext_start, ext.seqnum,
                      ext.offset, ext.len, adjust);
                XLOGF(DBG3, "Map: {}", co_await to_string());
                co_return {{offset, S3Ext{0, 0, len}}};
            }

            usize obj_adjust = adjust, img_adjust = adjust;
            if (adjust >= UINT32_MAX && ext.seqnum == 0)
                obj_adjust = 0;

            ENSURE(obj_adjust < UINT32_MAX);
            auto obj_off = ext.offset + (u32)(obj_adjust);
            auto img_off = ext_start + img_adjust;

            if (bytes_remaining <= ext.len) { // case a
                res.push_back({img_off, S3Ext{
                                            .seqnum = ext.seqnum,
                                            .offset = obj_off,
                                            .len = bytes_remaining,
                                        }});
                co_return res;
            } else { // case b
                res.push_back({img_off, S3Ext{
                                            .seqnum = ext.seqnum,
                                            .offset = obj_off,
                                            .len = ext.len,
                                        }});
                bytes_found += ext.len;
                it++;
                continue;
            }
        }

        // case 2. once again, two possibilities: bytes_remaining is either
        // a. (<= ext.len) or b. (> ext.len)

        // a. We have more extent remaining than we need, chop it off and return
        if (bytes_remaining <= ext.len) {
            res.push_back({ext_start, S3Ext{
                                          .seqnum = ext.seqnum,
                                          .offset = ext.offset,
                                          .len = bytes_remaining,
                                      }});
            co_return res;
        }

        // b. We still need more, add the current one and look or more
        res.push_back({ext_start, ext});
        bytes_found += ext.len;
        it++;
    }

    co_return res;
}

void ExtMap::unmap_locked(usize offset, usize len)
{
    auto start = offset;
    auto end = offset + len;
    // Find first extent where its base is > offset
    auto it = map.upper_bound(offset);
    it--; // go back one for <=

    // Check for case 1 where we bisect an existing extent
    // |----- existing extent -----------|
    //          |--- update ---|
    // |--left--|              |--right--|
    if (it->first < start && it->first + it->second.len > end) {
        auto &[base, left_ext] = *it;

        auto right_len = base + left_ext.len - end;
        auto right_off = left_ext.offset + (left_ext.len - right_len);
        map[end] = S3Ext{
            .seqnum = left_ext.seqnum,
            .offset = static_cast<u32>(right_off),
            .len = right_len,
        };

        left_ext.len = start - base;
        return;
    }

    // Case 2 We partially overlap with one or more existing extents
    // |------|--- existing ---|-------
    // |-------- update --------------|

    // Shrink the extent before the offset to end at the new extent start
    // ----- existing ----|
    // --------offset---|---len---|
    // -------- new ----|
    if (it->first < start) {
        auto &[base, ext] = *it;
        map[base].len = offset - base;
        it++;
    }

    // Remove all extents that are fully contained in the new extent
    // BUG: not sure how
    while (it != map.end() && it->first < end) {
        auto &[base, ext] = *it;
        if (base + ext.len <= end)
            it = map.erase(it);
        else
            break;
    }

    // Shrink the extent after the offset to start at the new extent end
    // -----------------|       |----existing ---
    // --------offset---|---len---|
    // -----------------|         |---- new ---
    if (it != map.end() && it->first < end) {
        auto [base, ext] = *it;
        map.erase(it);

        auto shrink_bytes = end - base;
        ENSURE(shrink_bytes < ext.len);
        ENSURE(shrink_bytes < UINT32_MAX);
        map[base + shrink_bytes] = S3Ext{
            .seqnum = ext.seqnum,
            .offset = ext.offset + static_cast<uint32_t>(shrink_bytes),
            .len = ext.len - shrink_bytes,
        };
    }
}

Task<void> ExtMap::update(usize base, usize len, S3Ext ext)
{
    ENSURE(ext.len == len);
    auto l = co_await mtx.co_scoped_lock();
    unmap_locked(base, len);
    map[base] = ext;

    if (VERIFY_MAP_INTEGRITY_ON_UPDATE)
        verify_integrity();
}

Task<void> ExtMap::unmap(usize base, usize len)
{
    auto l = co_await mtx.co_scoped_lock();
    unmap_locked(base, len);
    map[base] = {0, 0, len};

    // merge the extents before and after if they're both also unmapped
    // auto it = map.find(base);
    // ENSURE(it != map.end());
}

void ExtMap::verify_integrity()
{
    usize last_base = 0, last_len = 0, total = 0;
    for (auto &[base, ext] : map) {
        ENSURE(base >= last_base);
        ENSURE(base == last_base + last_len);
        last_base = base;
        last_len = ext.len;
        total += ext.len;
    }
    ENSURE(total == total_len);
}

Task<vec<byte>> ExtMap::serialise()
{
    vec<byte> buf;
    buf.reserve(map.size() * (sizeof(usize) + sizeof(S3Ext)));
    zpp::bits::out ar(buf);
    auto lck = co_await mtx.co_scoped_lock_shared();
    auto res = ar(map);
    // TODO handle error
    ENSURE(zpp::bits::failure(res) == false);
    co_return buf;
}

uptr<ExtMap> ExtMap::deserialise(vec<byte> buf)
{
    std::map<usize, S3Ext> map;
    zpp::bits::in ar(buf);
    auto res = ar(map);
    // TODO handle error
    ENSURE(zpp::bits::failure(res) == false);
    XLOGF(INFO, "Deserialised map with {} entries", map.size());
    return std::unique_ptr<ExtMap>(new ExtMap(std::move(map)));
}

uptr<ExtMap> ExtMap::create_empty(usize len)
{
    return std::unique_ptr<ExtMap>(new ExtMap(len));
}

Task<std::string> ExtMap::to_string()
{
    auto l = co_await mtx.co_scoped_lock();
    fvec<std::string> entries;
    for (auto &[base, ext] : map) {
        auto s =
            fmt::format("Extent: {:#x} -> {:#x}: (seqnum: {:#x}, "
                        "offset: {:#x}, len: {:#x})",
                        base, base + ext.len, ext.seqnum, ext.offset, ext.len);
        entries.push_back(s);
    }
    co_return folly::join("\n", entries);
}
