#pragma once

#include <array>
#include <boost/bimap.hpp>
#include <map>
#include <mutex>
#include <shared_mutex>

#include "backend.h"
#include "extent.h"
#include "nvme.h"
#include "request.h"
#include "smartiov.h"
#include "utils.h"

const size_t CACHE_CHUNK_SIZE = 64 * 1024;
const size_t CACHE_HEADER_SIZE = 4096;
using cache_chunk = std::array<char, CACHE_CHUNK_SIZE>;
using chunk_idx = size_t;

using chunk_key = std::tuple<std::string, uint64_t, size_t>;

/**
 * This is a cache in front of the backend. It's indexed by
 * <objname, seqnum, offset>, where offset is aligned to CACHE_CHUNK_SIZE.
 *
 * The cache is fixed size, and cache chunks are allocated the pool of
 * all blocks.
 *
 * The lifecycle of a request is:
 * - We get a read request
 * - We check if it's in the cache
 * - If so, we return the cached data
 * - Otherwise, we check if there's an outstanding fetch for the data
 * - If so, we wait for the fetch to complete
 * - Otherwise, we fetch the data from the backend
 * - Once the backend request completes, we dispatch pending reads and write
 *   to the cache
 *
 * Important to note is that the cache is currently a giant file, and read/write
 * is done by nvme_uring. An alternative would be to use a giant mmap'ed file,
 * or to be purely memory-backed.
 *
 * We might also want to think about adding an in-memory cache to this instead
 * of just relying on the kernel page cache as the memory cache.
 */
class shared_read_cache
{
  private:
    class cache_hit_request;
    class cache_miss_request;
    class pending_read_request;

    enum entry_status {
        EMPTY,
        FETCHING,
        FILLING,
        VALID,
    };

    struct entry_state {
        entry_status status = EMPTY;

        bool accessed = false;

        // We use this to count how many requests refer to this chunk.
        // If non-zero, we cannot evict this chunk
        int32_t refcount = 0;

        // This is used when the backend request has come back and we're now
        // going to fill it into the cache. Status must be FILLING.
        // NULL at all other times
        // void *pending_fill_data = nullptr;

        // Keep track of pending reads
        std::vector<pending_read_request *> pending_reads;
    };

    std::vector<entry_state> cache_state;

    std::shared_mutex global_cache_lock;

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
    boost::bimap<chunk_key, chunk_idx> cache_map;

    // clock eviction
    size_t clock_idx = 0;

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

  public:
    shared_read_cache(std::string cache_path, size_t num_cache_chunks,
                      sptr<backend> obj_backend);
    ~shared_read_cache();

    /**
     * The shared read cache should be *shared*, so we use a singleton
     * The passed in params are only used on 1st call to construct the cache;
     * it's ignored on all subsequent calls
     */
    static sptr<shared_read_cache> get_instance(std::string cache_path,
                                                size_t num_cache_blocks,
                                                sptr<backend> obj_backend);

    /**
     * Read a single cache chunk. obj_offset MUST be cache block aligned, and
     * will read `iov.bytes` bytes from the cache block into the dest smartiov,
     * starting at `obj_offset + adjust`. `adjust + iov.bytes` MUST be less than
     * the size of a cache block.
     *
     * If the data is not in the cache, it will be fetched from the backend.
     * See documentation for shared_read_cache for more details on the lifecycle
     * of a request.
     */
    request *make_read_req(std::string img_prefix, uint64_t seqnum,
                           size_t obj_offset, size_t adjust, smartiov &dest);
};
