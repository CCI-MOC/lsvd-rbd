#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/framework/extractor.hpp>
#include <boost/accumulators/statistics.hpp>
#include <boost/accumulators/statistics/rolling_count.hpp>
#include <boost/accumulators/statistics/rolling_sum.hpp>
#include <boost/bimap.hpp>
#include <boost/container_hash/hash.hpp>
#include <folly/experimental/coro/SharedMutex.h>
#include <
#include <thread>

#include "backend.h"
#include "folly/Synchronized.h"
#include "lsvd_types.h"
#include "nvme.h"
#include "read_cache.h"
#include "subprojects/folly/folly/container/FBVector.h"

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

using namespace boost::accumulators;
const size_t CACHE_STATS_WINDOW = 10'000;

class read_cache_shard_coro : public read_cache_coro
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

    folly::fbvector<entry_state> cache_state;
    folly::coro::SharedMutex cache_lock;

    int fd;
    uptr<nvme> cache_store;
    size_t size_in_chunks;
    size_t header_size_bytes;
    size_t cache_filesize_bytes;
    sptr<backend> obj_backend;

    // cache map
    // we map <objname, seqnum, offset> to a cache block
    // offset MUST be a multiple of CACHE_CHUNK_SIZE
    // the reverse map exists so that we can evict entries
    // boost::bimap<chunk_key, chunk_idx> cache_map;
    std::unordered_map<chunk_key, chunk_idx, chunk_key_hash> cache_map;

    // clock eviction
    size_t clock_idx = 0;

    // maintain cache hit rate statistics
    std::atomic_int total_requests = 0;
    accumulator_set<int, stats<tag::rolling_sum, tag::rolling_count>>
        hitrate_stats;
    accumulator_set<size_t, stats<tag::rolling_sum>> user_bytes;
    accumulator_set<size_t, stats<tag::rolling_sum>> backend_bytes;

    /**
     * Allocate a new cache chunk, and return its index
     *
     * This uses the clock eviction policy; it will search through the cache
     * in index order, and clear abit it is set and evict the first entry
     * without abit set
     */
    chunk_idx allocate_chunk();

    void dec_chunk_refcount(chunk_idx idx);
    entry_status get_chunk_status(chunk_idx idx);
    void set_chunk_status(chunk_idx idx, entry_status status);

    size_t get_store_offset_for_chunk(chunk_idx idx);
    request *get_fill_req(chunk_idx idx, void *data);

    void on_store_done(chunk_idx idx);

  public:
    read_cache_shard_coro(std::string cache_path, size_t num_cache_chunks,
                          sptr<backend> obj_backend);
    ~read_cache_shard_coro() {}

    // because the part that does the request slicing is not here, we need to
    // let the upper layers hint to us that there was a bypassed request
    bool should_bypass_cache();
    void served_bypass_request(size_t bytes);

    /**
     * Read a single cache chunk. obj_offset MUST be cache block aligned, and
     * will read `iov.bytes` bytes from the cache block into the dest smartiov,
     * starting at `obj_offset + adjust`. `adjust + iov.bytes` MUST be less than
     * the size of a cache block.
     *
     * If the data is not in the cache, it will be fetched from the backend.
     * See documentation for shared_read_cache for more details on the lifecycle
     * of a request.
     *
     * If data is in memory, this will fill in the iov and return NULL
     */
    ResTask<void> read(str img, seqnum_t seqnum, usize offset, usize adjust,
                       smartiov &dest)
    {

        auto req_size = dest.bytes();

        assert(offset % CACHE_CHUNK_SIZE == 0);
        assert(adjust + req_size <= CACHE_CHUNK_SIZE);
    }

    /**
     * Insert a backend object into the cache.
     */
    void insert_object(chunk_key key, void *data);

    std::tuple<size_t, size_t, size_t, size_t, size_t> get_stats()
    {
        std::unique_lock lock(cache_stats_lock);
        return std::make_tuple(
            rolling_sum(user_bytes), rolling_sum(backend_bytes),
            rolling_sum(hitrate_stats), rolling_count(hitrate_stats),
            (size_t)total_requests);
    }
};