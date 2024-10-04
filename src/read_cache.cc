#include "read_cache.h"
#include "backend.h"
#include "cachelib/allocator/CacheAllocator.h"
#include "representation.h"
#include "smartiov.h"
#include "utils.h"

using Cache = facebook::cachelib::LruAllocator;
using PoolId = facebook::cachelib::PoolId;

struct iov {
    byte *buf;
    usize len;
};

class SharedCache
{
    uptr<Cache> cache;
    PoolId pid;

    SharedCache(uptr<Cache> cache_) : cache(std::move(cache_))
    {
        pid = cache->addPool("default",
                             cache->getCacheMemoryStats().ramCacheSize);
    }

    inline static sptr<SharedCache> singleton = nullptr;

  public:
    static void init_cache(usize mem_bytes, usize nvm_bytes, fstr nvm_path)
    {
        XLOGF(INFO,
              "Initialising SharedCache with {} bytes of memory and {} "
              "bytes of NVM cache at {}",
              mem_bytes, nvm_bytes, nvm_path);
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
        singleton = sptr<SharedCache>(new SharedCache(std::move(c)));
    }

    static sptr<SharedCache> get()
    {
        if (!singleton) [[unlikely]] {
            XLOGF(FATAL, "SharedCache not initialized");
            std::abort();
        }
        return singleton;
    }

    // returns true iff item was found
    auto read(strv key, usize adjust, smartiov dest) -> ResTask<usize>
    {
        auto h = co_await cache->find(key).toSemiFuture();
        if (!h)
            co_return outcome::failure(std::errc::no_such_file_or_directory);

        auto copy_len = std::min(h->getSize() - adjust, dest.bytes());
        dest.copy_in((byte *)h->getMemory() + adjust, copy_len);
        co_return copy_len;
    }

    auto insert(strv key, buffer src) -> Task<bool>
    {
        auto h = cache->allocate(pid, key, src.len);
        if (!h) [[unlikely]] {
            XLOGF(ERR, "Failed to allocate {} bytes for key: {}", src.len, key);
            co_return false;
        }
        std::memcpy(h->getMemory(), src.buf, src.len);
        cache->insertOrReplace(h);
        co_return true;
    }
};

class ImageObjCache : public ReadCache
{
    inline static const usize CACHE_CHUNK_SIZE = 128 * 1024;

    fstr imgname;
    sptr<ObjStore> s3;
    sptr<SharedCache> cache;

  public:
    ImageObjCache(fstr imgname, sptr<ObjStore> s3, sptr<SharedCache> cache)
        : imgname(imgname), s3(s3), cache(cache)
    {
    }

    auto get_key(seqnum_t seq, usize offset) -> fstr
    {
        ENSURE(offset % CACHE_CHUNK_SIZE == 0);
        return get_cache_key(imgname, seq, offset);
    }

    auto read_chunk(seqnum_t seq, usize off, usize adjust,
                    smartiov dest) -> ResTask<void>
    {
        auto cache_key = get_key(seq, off);
        auto get_res = co_await cache->read(cache_key, adjust, dest);
        if (get_res.has_value())
            co_return outcome::success();

        // cache miss, fetch from backend
        auto obj_key = get_logobj_key(imgname, seq);
        vec<byte> chunk_buf(CACHE_CHUNK_SIZE);
        auto siov = smartiov::from_buf(chunk_buf);
        auto fetch_len =
            BOOST_OUTCOME_CO_TRYX(co_await s3->read(obj_key, off, siov));

        if (fetch_len == 0) [[unlikely]] {
            XLOGF(ERR, "0-length read for key '{}', off {}, len {}", cache_key,
                  off, siov.bytes());
            co_return outcome::failure(std::errc::no_message);
        }
        if (adjust + dest.bytes() > fetch_len) [[unlikely]] {
            XLOGF(ERR,
                  "Read past end of chunk: key {} adj {} len {}; found {}/{} ",
                  cache_key, adjust, dest.bytes(), fetch_len,
                  adjust + dest.bytes());
            co_return outcome::failure(std::errc::value_too_large);
        }

        // insert into cache and return
        co_await cache->insert(cache_key, buffer{chunk_buf.data(), fetch_len});
        dest.copy_in(chunk_buf.data() + adjust, dest.bytes());

        co_return outcome::success();
    }

    ResTask<void> read(S3Ext ext, smartiov dest) override
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
            auto adjust = (ext.offset + bytes_read) % CACHE_CHUNK_SIZE;
            auto chunk_off = ext.offset + bytes_read - adjust;
            auto to_read =
                std::min(CACHE_CHUNK_SIZE - adjust, ext.len - bytes_read);
            auto iov = dest.slice(bytes_read, to_read);
            tasks.push_back(
                read_chunk(ext.seqnum, chunk_off, adjust, iov).semi());
            bytes_read += to_read;
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
        for (usize off = 0; off < iov.len; off += CACHE_CHUNK_SIZE) {
            auto k = get_key(seqnum, off);
            auto size = std::min(CACHE_CHUNK_SIZE, iov.len - off);
            // TODO figure out what to do for failure
            // In theory, it doesn't really matter if we fail since we'll miss
            // the next time and fetch it from backend
            co_await cache->insert(k, buffer{iov.buf + off, size});
        }

        co_return outcome::success();
    }
};

void ReadCache::init_cache(usize mem_bytes, usize nvm_bytes, fstr nvm_path)
{
    SharedCache::init_cache(mem_bytes, nvm_bytes, nvm_path);
}

uptr<ReadCache> ReadCache::make_image_cache(sptr<ObjStore> s3, fstr imgname)
{
    return std::make_unique<ImageObjCache>(imgname, s3, SharedCache::get());
}
