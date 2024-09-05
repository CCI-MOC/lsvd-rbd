#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/framework/extractor.hpp>
#include <boost/accumulators/statistics.hpp>
#include <boost/accumulators/statistics/rolling_count.hpp>
#include <boost/accumulators/statistics/rolling_sum.hpp>
#include <boost/bimap.hpp>
#include <boost/container_hash/hash.hpp>
#include <folly/FBVector.h>
#include <folly/experimental/coro/SharedMutex.h>

#include "backend.h"
#include "folly/container/F14Map.h"
#include "lsvd_types.h"
#include "nvme.h"
#include "read_cache.h"

template <typename T> using fvec = folly::fbvector<T>;

const size_t CACHE_CHUNK_SIZE = 64 * 1024;
const size_t CACHE_HEADER_SIZE = 4096;
using chunk_idx = size_t;

/**
 * This is the key for the cache map. It's a tuple of <objname, seqnum, offset>
 */
using chunk_key = std::tuple<std::string, uint64_t, size_t>;
struct chunk_key_hash {
    inline size_t operator()(const chunk_key &k) const
    {
        size_t seed = 0;
        boost::hash_combine(seed, std::get<0>(k));
        boost::hash_combine(seed, std::get<1>(k));
        boost::hash_combine(seed, std::get<2>(k));
        return seed;
    }
};

class read_cache_shard_coro
{
  private:
    enum entry_status {
        EMPTY,
        FETCHING,
        FILLING,
        VALID,
    };

    struct entry_state {
        entry_status status = EMPTY;

        std::atomic_bool accessed = false;

        // We use this to count how many requests refer to this chunk.
        // If non-zero, we cannot evict this chunk
        std::atomic<u32> refcount = 0;

        // This is used when the backend request has come back and we're now
        // going to fill it into the cache. Status must be FILLING.
        // NULL at all other times
        void *pending_fill_data = nullptr;

        // Keep track of pending reads
        // vec<pending_read_request *> pending_reads;

        // Keep track of the reverse map so we can evict this entry
        chunk_key key;
    };

    fvec<entry_state> cache_state;
    folly::coro::SharedMutex cache_lock;

    uptr<fileio> cache_file;
    sptr<backend_coro> s3;

    folly::coro::SharedMutex map_lock;
    folly::F14FastMap<chunk_key, usize> entry_map;

  public:
    read_cache_shard_coro(str cache_path, usize num_cache_chunks,
                          sptr<backend_coro> s3)
        : s3(s3)
    {
        auto fd = open(cache_path.c_str(), O_RDWR | O_CREAT, 0666);
        cache_file = make_fileio(fd);
    }

    ~read_cache_shard_coro() {}

    size_t get_store_offset_for_chunk(chunk_idx chunk)
    {
        return chunk * CACHE_CHUNK_SIZE + CACHE_HEADER_SIZE;
    }

    auto allocate_chunk() -> usize;

    ResTask<void> read(str img, seqnum_t seqnum, usize offset, usize adjust,
                       smartiov &dest)
    {
        auto req_size = dest.bytes();
        assert(offset % CACHE_CHUNK_SIZE == 0);
        assert(adjust + req_size <= CACHE_CHUNK_SIZE);

        auto key = chunk_key{img, seqnum, offset};

        // TODO figure out locking for this
        auto v = entry_map.find(key);

        // cache hit
        if (v != entry_map.end()) {
            auto idx = v->second;
            auto &entry = cache_state.at(idx);
            entry.refcount += 1;
            entry.accessed = true;

            // valid entry, read and return
            if (entry.status == entry_status::VALID) {
                auto offset = get_store_offset_for_chunk(idx);
                BOOST_OUTCOME_CO_TRYX(
                    co_await cache_file->read(offset + adjust, dest));
                co_return outcome::success();
            }

            // TODO other states
        }

        // cache miss
        auto idx = allocate_chunk();
        auto &entry = cache_state.at(idx);
        entry.refcount += 1;

        // read from backend
        auto buf = fvec<byte>(CACHE_CHUNK_SIZE);
        auto oname = objname(img, seqnum);
        std::ignore = BOOST_OUTCOME_CO_TRYX(co_await s3->read(
            oname.str(), offset, iovec{buf.data(), buf.size()}));
        // todo handle partial writes

        // write to cache
        auto cache_offset = get_store_offset_for_chunk(idx);
        std::ignore = BOOST_OUTCOME_CO_TRYX(co_await cache_file->write(
            cache_offset, iovec{buf.data(), buf.size()}));
        // todo handle partial writes

        co_return outcome::success();
    }

    /**
     * Insert a backend object into the cache.
     */
    void insert_object(chunk_key key, void *data);
};