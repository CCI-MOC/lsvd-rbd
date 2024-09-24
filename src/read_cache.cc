#include "read_cache.h"
#include "backend.h"
#include "cachelib/allocator/CacheAllocator.h"
#include "representation.h"
#include "smartiov.h"
#include "utils.h"

using Cache = facebook::cachelib::Lru2QAllocator;
using PoolId = facebook::cachelib::PoolId;

struct iov {
    byte *buf;
    usize len;
};

class SharedCache
{
    uptr<Cache> cache;
    PoolId pid;

    SharedCache(uptr<Cache> cache) : cache(std::move(cache))
    {
        pid = cache->addPool("default",
                             cache->getCacheMemoryStats().ramCacheSize);
    }

  public:
    static auto make_cache(usize mem_bytes, usize nvm_bytes, fstr nvm_path)
    {
        Cache::NvmCacheConfig ncfg;
        ncfg.navyConfig.setBlockSize(4096);
        ncfg.navyConfig.setSimpleFile(nvm_path.toStdString(), nvm_bytes, true);
        ncfg.navyConfig.blockCache().setRegionSize(16 * 1024 * 1024);
        ncfg.navyConfig.enableAsyncIo(0, true);

        Cache::Config cfg;
        cfg.setCacheName("LSVD Shared Object Cache");
        cfg.enableNvmCache(ncfg);
        cfg.setCacheSize(mem_bytes);
        cfg.validate();

        auto c = std::make_unique<Cache>(cfg);
        auto sc = new SharedCache(std::move(c));
        return std::unique_ptr<SharedCache>(sc);
    }

    // returns true iff item was found
    auto read(strv key, usize adjust, smartiov &dest) -> Task<bool>
    {
        auto h = co_await cache->find(key).toSemiFuture();

        if (!h)
            co_return false;

        if (dest.bytes() > h->getSize() + adjust) {
            XLOG(ERR,
                 "Object too small for read. key: {}, adjust {} cache entry "
                 "size: {}, buffer size: {}",
                 key, adjust, h->getSize(), dest.bytes());
            co_return false;
        }

        dest.copy_in((byte *)h->getMemory() + adjust, dest.bytes());
        co_return true;
    }

    auto insert(strv key, buffer src) -> Task<bool>
    {
        auto h = cache->allocate(pid, key, src.len);
        if (!h) {
            XLOG(ERR, "Failed to allocate {} bytes for key: {}", src.len, key);
            co_return false;
        }
        std::memcpy(h->getMemory(), src.buf, src.len);
        cache->insertOrReplace(h);
        co_return true;
    }
};

class ImageObjCache : public ReadCache
{
    static const usize chunk_size = 128 * 1024;

    fstr imgname;
    sptr<ObjStore> s3;
    sptr<SharedCache> cache;

    auto get_key(seqnum_t seq, usize offset) -> fstr
    {
        assert(offset % chunk_size == 0);
        return get_cache_key(imgname, seq, offset);
    }

    auto read_chunk(seqnum_t seq, usize offset, usize adjust,
                    smartiov &dest) -> ResTask<void>
    {
        auto k = get_key(seq, offset);
        auto in_cache = co_await cache->read(k, adjust, dest);
        if (in_cache)
            co_return outcome::success();

        vec<byte> chunk_buf;
        auto siov = smartiov::from_buf(chunk_buf);
        auto n = BOOST_OUTCOME_CO_TRYX(co_await s3->read(k, offset, siov));
        if (n == 0)
            co_return outcome::failure(std::errc::no_such_file_or_directory);

        co_await cache->insert(k, buffer{chunk_buf.data(), chunk_buf.size()});
        dest.copy_in(chunk_buf.data() + adjust, dest.bytes());
        co_return outcome::success();
    }

    ResTask<void> read(S3Ext ext, smartiov &dest) override
    {
        /**
        |-----------------------entire object---------------------------------|
        |-----chunk1-----|-----chunk2-----|-----chunk3-----|-----chunk4-----|
        |-------- ext.offset --------|---ext.len---|
        |----chunk_off---|---adjust--|----|
                             to_read --^

        - Adjust is the offset in bytes from beginning of the chunk
         */
        fvec<folly::SemiFuture<Result<void>>> tasks;
        for (usize bytes_read = 0; bytes_read < ext.len;) {
            auto adjust = (ext.offset + bytes_read) % chunk_size;
            auto chunk_off = ext.offset + bytes_read - adjust;
            auto to_read = std::min(chunk_size - adjust, ext.len - bytes_read);
            auto iov = dest.slice(bytes_read, bytes_read + to_read);
            tasks.push_back(
                read_chunk(ext.seqnum, chunk_off, adjust, iov).semi());
        }

        auto all = co_await folly::collectAll(tasks);
        for (auto &t : all)
            if (t.hasException())
                co_return outcome::failure(std::errc::io_error);
            else
                BOOST_OUTCOME_CO_TRYX(t.value());
        co_return outcome::success();
    }

    ResTask<void> insert_obj(seqnum_t seqnum, buffer iov) override
    {
        // split into chunks of size chunk_size each, and then insert them
        // individually into the cache
        for (usize off = 0; off < iov.len; off += chunk_size) {
            auto k = get_key(seqnum, off);
            auto size = std::min(chunk_size, iov.len - off);
            // TODO figure out what to do for failure
            // In theory, it doesn't really matter if we fail since we'll miss
            // the next time and fetch it from backend
            co_await cache->insert(k, buffer{iov.buf + off, size});
        }

        co_return outcome::success();
    }
};
