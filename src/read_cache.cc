#include <cachelib/allocator/CacheAllocator.h>

#include "read_cache.h"

using Cache = facebook::cachelib::LruAllocator;
using CachePoolId = facebook::cachelib::PoolId;

class CachelibWrap : public ReadCache
{
    uptr<Cache> cache;
    CachePoolId pid;

    ResTask<void> read(S3Ext ext, smartiov &dest) = 0;

    ResTask<void> insert_obj(seqnum_t seqnum, iovec iov)
    {
        auto whandle = cache->allocate(pid, "key1", iov.iov_len);
        std::memcpy(whandle->getMemory(), iov.iov_base, iov.iov_len);
        cache->insert(whandle);
        co_return outcome::success();
    }
};

class UringCache : public ReadCache
{
};
