#include <folly/experimental/coro/SharedMutex.h>
#include <map>
#include <unistd.h>
#include <utility>

#include "representation.h"
#include "utils.h"

#ifndef VERIFY_MAP_INTEGRITY_ON_UPDATE
#define VERIFY_MAP_INTEGRITY_ON_UPDATE false
#endif

/**
Maps a range of addresses to an extent on the backend.

Some of these extents may actually point to objects not yet written to the
backend and thus only exist in memory or write log. This is fine, however, as
each extent can only point to one object, and an object will always be either
wholly on the backend or not.

As such, we don't need to maintain a separate map for pending writes, we just
need to search first to see if we have a copy of the object in cache before
going off to the backend.

When checkpointing, the checkpoint is always between two different backend
objects, so we can guarantee that, at the time of freezing the map for read,
that all extents in the map are sequentially before the checkpoint.
*/
class ExtMap
{
    folly::coro::SharedMutex mtx;

    /**
    We try to map the entire range of addresses so we can always assume that
    we'll find something for all ranges that are looked up. If we don't, that's
    a sign of map corruption.

    Unmapped ranges are represented by an extent with seqnum = 0. Offsets for
    extents with seqnum = 0 are ignored and can be of any value
     */
    std::map<usize, S3Ext> map;
    usize total_len = 0;

    // Internal use, assumes the lock is already held
    void unmap_locked(usize base, usize len);
    void verify_integrity();

    ExtMap(usize len) : total_len(len) { map.insert({0, S3Ext{0, 0, len}}); }
    ExtMap(std::map<usize, S3Ext> map_) : map(std::move(map_))
    {
        for (auto &[base, ext] : map)
            total_len += ext.len;
    }

  public:
    ~ExtMap() {}

    Task<vec<std::pair<usize, S3Ext>>> lookup(usize offset, usize len);
    Task<void> update(usize base, usize len, S3Ext val);
    Task<void> unmap(usize base, usize len);

    Task<std::string> to_string();
    Task<vec<byte>> serialise();
    static uptr<ExtMap> deserialise(vec<byte> buf);

    static uptr<ExtMap> create_empty(usize len);
};
