#include "absl/status/status.h"
#include "cachelib/allocator/CacheAllocator.h"
#include "fmt/chrono.h"
#include "fmt/os.h"

#include "backend.h"
#include "config.h"
#include "read_cache.h"
#include "representation.h"
#include "smartiov.h"
#include "utils.h"

FOLLY_GFLAGS_DECLARE_string(lsvd_journ_dir);
FOLLY_GFLAGS_DEFINE_int64(lsvd_stats_interval, 10'000,
                          "Interval for reporting cache statistics");
FOLLY_GFLAGS_DEFINE_bool(lsvd_report_cache_stats, true,
                         "Interval for reporting cache statistics");

using Cache = facebook::cachelib::TinyLFUAllocator;
using PoolId = facebook::cachelib::PoolId;

struct iov {
    byte *buf;
    usize len;
};

class SharedCache
{
    uptr<Cache> cache;
    PoolId pid;
    fmt::ostream shared_stats_f;
    fmt::ostream img_stats_f;

    // stats
    std::atomic<u64> num_shared_read = 0;
    std::atomic<u64> num_shared_hit = 0;
    std::atomic<u64> num_all_ops = 0;

    SharedCache(uptr<Cache> cache_, fmt::ostream shared_f, fmt::ostream img_f)
        : cache(std::move(cache_)), shared_stats_f(std::move(shared_f)),
          img_stats_f(std::move(img_f))
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

        auto t = std::time(nullptr);
        auto now_ts = fmt::format("{:%Y%m%d_%H%M%S}", fmt::localtime(t));

        auto c = std::make_unique<Cache>(cfg);
        auto shared_stats_f = fmt::output_file(
            FLAGS_lsvd_journ_dir + "/shared_cache_stats" + now_ts + ".txt");
        auto img_stats_f = fmt::output_file(
            FLAGS_lsvd_journ_dir + "/img_cache_stats" + now_ts + ".txt");

        img_stats_f.print("time,imgname,reads,chunks,misses\n");
        shared_stats_f.print(
            "time,reads,chunks,hits,misses,hit_ratio,ram,nvm\n");

        singleton = sptr<SharedCache>(new SharedCache(
            std::move(c), std::move(shared_stats_f), std::move(img_stats_f)));
    }

    static void shutdown_cache()
    {
        if (singleton)
            singleton->cache.reset();
        else
            XLOGF(ERR, "SharedCache not initialized");
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
    auto read(strv key, usize adjust, smartiov dest) -> TaskRes<usize>
    {
        num_shared_read.fetch_add(1);
        auto h = co_await cache->find(key).toSemiFuture();
        // auto h = cache->find(key);
        if (!h)
            co_return absl::NotFoundError(key);

        num_shared_hit.fetch_add(1);
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
        co_await cache->insertOrReplace(h).toSemiFuture();
        co_return true;
    }

    auto log_img_stats(fstr imgname, u64 reads, u64 chunks, u64 misses)
    {
        auto now_us = get_now_us();
        img_stats_f.print("{},{},{},{},{}\n", now_us, imgname, reads, chunks,
                          misses);
    }

    auto register_imgread()
    {
        auto total_ops = num_all_ops.fetch_add(1);

        if (total_ops % FLAGS_lsvd_stats_interval != 1)
            return;

        auto now_us = get_now_us();
        auto stats = cache->getCacheMemoryStats();
        auto chunks = num_shared_read.load();
        auto hits = num_shared_hit.load();
        auto misses = chunks - hits;
        auto hit_ratio = (chunks == 0) ? 0.0 : (f64(hits) / chunks);

        shared_stats_f.print("{},{},{},{},{},{},{},{}\n", now_us, total_ops,
                             chunks, hits, misses, hit_ratio,
                             stats.ramCacheSize, stats.nvmCacheSize);
    }
};

class ImageObjCache : public ReadCache
{
    inline static const usize CACHE_CHUNK_SIZE = 128 * 1024;

    fstr imgname;
    sptr<ObjStore> s3;
    sptr<SharedCache> cache;

    std::atomic<u64> num_reads = 0;
    std::atomic<u64> num_chunks_reads = 0;
    std::atomic<u64> num_chunks_miss = 0;

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
                    smartiov dest) -> TaskUnit
    {
        cache->register_imgread();
        num_chunks_reads.fetch_add(1);

        auto cache_key = get_key(seq, off);
        auto get_res = co_await cache->read(cache_key, adjust, dest);
        if (get_res.ok())
            co_return folly::Unit();

        // cache miss, fetch from backend
        num_chunks_miss.fetch_add(1);

        auto obj_key = get_logobj_key(imgname, seq);
        vec<byte> chunk_buf(CACHE_CHUNK_SIZE);
        auto siov = smartiov::from_buf(chunk_buf);

        auto fetch_res = co_await s3->read(obj_key, off, siov);
        CRET_IF_NOTOK(fetch_res);
        auto fetch_len = *fetch_res;

        if (fetch_len == 0) [[unlikely]] {
            XLOGF(ERR, "0-length read for key '{}', off {}, len {}", cache_key,
                  off, siov.bytes());
            co_return absl::OutOfRangeError(cache_key);
        }
        if (adjust + dest.bytes() > fetch_len) [[unlikely]] {
            XLOGF(ERR,
                  "Read past end of chunk: key {} adj {} len {}; found {}/{} ",
                  cache_key, adjust, dest.bytes(), fetch_len,
                  adjust + dest.bytes());
            co_return absl::OutOfRangeError(cache_key);
        }

        // insert into cache and return
        co_await cache->insert(cache_key, buffer{chunk_buf.data(), fetch_len});
        dest.copy_in(chunk_buf.data() + adjust, dest.bytes());

        co_return folly::Unit();
    }

    TaskUnit read(S3Ext ext, smartiov dest) override
    {
        num_reads.fetch_add(1);
        if (FLAGS_lsvd_report_cache_stats &&
            num_reads % FLAGS_lsvd_stats_interval == 1)
            cache->log_img_stats(imgname, num_reads.load(),
                                 num_chunks_reads.load(),
                                 num_chunks_miss.load());
        /**
        |-----------------------entire object---------------------------------|
        |-----chunk1-----|-----chunk2-----|-----chunk3-----|-----chunk4-----|
        |-------- ext.offset --------|---ext.len---|
        |----chunk_off---|---adjust--|----|
                             to_read --^

        - Adjust is the offset in bytes from beginning of the chunk
         */
        fvec<folly::SemiFuture<Result<folly::Unit>>> tasks;
        for (usize bytes_read = 0; bytes_read < ext.len;) {
            auto adjust = (ext.offset + bytes_read) % CACHE_CHUNK_SIZE;
            auto chunk_off = ext.offset + bytes_read - adjust;
            auto to_read =
                std::min(CACHE_CHUNK_SIZE - adjust, ext.len - bytes_read);
            auto iov = dest.slice(bytes_read, to_read);

            if (ENABLE_SEQUENTIAL_DEBUG_READS) {
                auto res =
                    co_await read_chunk(ext.seqnum, chunk_off, adjust, iov);
                DEBUG_IF_FAIL(res);
            } else {
                tasks.push_back(
                    read_chunk(ext.seqnum, chunk_off, adjust, iov).semi());
            }

            bytes_read += to_read;
        }

        auto all = co_await folly::collectAll(tasks);
        for (auto &t : all)
            if (t.hasException())
                co_return absl::InternalError(t.exception().what());
            else if (!t.value().ok())
                co_return t->status();
        co_return folly::Unit();
    }

    TaskUnit insert_obj(seqnum_t seqnum, buffer iov) override
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

        co_return folly::Unit();
    }
};

void ReadCache::init_cache(usize mem_bytes, usize nvm_bytes, fstr nvm_path)
{
    SharedCache::init_cache(mem_bytes, nvm_bytes, nvm_path);
}

void ReadCache::shutdown_cache() { SharedCache::shutdown_cache(); }

uptr<ReadCache> ReadCache::make_image_cache(sptr<ObjStore> s3, fstr imgname)
{
    return std::make_unique<ImageObjCache>(imgname, s3, SharedCache::get());
}
