#include <cstdint>
#include <folly/logging/xlog.h>
#include <memory>

#include "extmap.h"
#include "zpp_bits.h"

Task<vec<std::pair<usize, S3Ext>>> ExtMap::lookup(usize offset, usize len)
{
    auto l = co_await mtx.co_scoped_lock_shared();
    vec<std::pair<usize, S3Ext>> res;

    // upper_bound returns the first element that is > offset, so the element
    // at which we start is always the one before that
    auto it = map.upper_bound(offset);

    // the whole address space should be mapped, so if we don't find anything
    // that means we're looking at a corrupted map
    if (it == map.end()) {
        XLOGF(ERR, "Corrupt map: no entry found for offset {}", offset);
        co_return res;
    }

    // Go back one so we start at the right place
    it--;

    usize bytes_found = 0;
    while (bytes_found < len) {
        auto &[base, ext] = *it;
        auto bytes_remaining = len - bytes_found;

        if (it == map.end() || base > offset + len ||
            base != offset + bytes_found) {
            XLOGF(ERR, "Corrupt map detected on {}+{}", offset, len);
            co_return res;
        }

        // We have more extent remaining than we need, chop it off and return
        if (ext.len > bytes_remaining) {
            res.push_back({base, S3Ext{
                                     .seqnum = ext.seqnum,
                                     .offset = ext.offset,
                                     .len = bytes_remaining,
                                 }});
            co_return res;
        }

        // We still need more, add the current one and look or more
        res.push_back({base, ext});
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
        auto &[base, ext] = *it;
        ext.len = start - base;

        auto right_len = base + ext.len - end;
        auto right_off = ext.offset + (ext.len - right_len);
        map[end] = S3Ext{
            .seqnum = ext.seqnum,
            .offset = static_cast<u32>(right_off),
            .len = right_len,
        };

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
    while (it != map.end() && it->first < end) {
        auto &[base, ext] = *it;
        if (base + ext.len < end)
            it = map.erase(it);
    }

    // Shrink the extent after the offset to start at the new extent end
    // -----------------|       |----existing ---
    // --------offset---|---len---|
    // -----------------|         |---- new ---
    if (it != map.end() && it->first < end) {
        auto [base, ext] = *it;
        map.erase(it);

        auto shrink_bytes = end - base;
        assert(shrink_bytes < ext.len);
        assert(shrink_bytes < UINT32_MAX);
        map[base + shrink_bytes] = S3Ext{
            .seqnum = ext.seqnum,
            .offset = ext.offset + static_cast<uint32_t>(shrink_bytes),
            .len = ext.len - shrink_bytes,
        };
    }
}

Task<void> ExtMap::update(usize base, usize len, S3Ext ext)
{
    assert(ext.len == len);
    auto l = co_await mtx.co_scoped_lock();
    unmap_locked(base, len);
    map[base] = ext;
}

Task<void> ExtMap::unmap(usize base, usize len)
{
    auto l = co_await mtx.co_scoped_lock();
    unmap_locked(base, len);
    map[base] = {0, 0, len};
}

Task<vec<byte>> ExtMap::serialise()
{
    vec<byte> buf;
    buf.reserve(map.size() * (sizeof(usize) + sizeof(S3Ext)));
    zpp::bits::out ar(buf);
    auto lck = co_await mtx.co_scoped_lock_shared();
    auto res = ar(map);
    // TODO handle error
    assert(zpp::bits::failure(res) == false);
    co_return buf;
}

uptr<ExtMap> ExtMap::deserialise(vec<byte> buf)
{
    std::map<usize, S3Ext> map;
    zpp::bits::in ar(buf);
    auto res = ar(map);
    // TODO handle error
    assert(zpp::bits::failure(res) == false);
    return std::make_unique<ExtMap>(map);
}
