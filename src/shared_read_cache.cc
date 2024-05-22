#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/framework/extractor.hpp>
#include <boost/accumulators/statistics.hpp>
#include <boost/accumulators/statistics/rolling_count.hpp>
#include <boost/accumulators/statistics/rolling_sum.hpp>
#include <boost/bimap.hpp>
#include <boost/container_hash/hash.hpp>
#include <thread>

#include "lsvd_types.h"
#include "nvme.h"
#include "shared_read_cache.h"

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
    class cache_insert_request;

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
        std::atomic_int32_t refcount = 0;

        // This is used when the backend request has come back and we're now
        // going to fill it into the cache. Status must be FILLING.
        // NULL at all other times
        void *pending_fill_data = nullptr;

        // Keep track of pending reads
        std::vector<pending_read_request *> pending_reads;

        // Keep track of the reverse map so we can evict this entry
        chunk_key key;
    };

    std::vector<entry_state> cache_state;

    std::mutex global_cache_lock;
    std::mutex cache_stats_lock;

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
    shared_read_cache(std::string cache_path, size_t num_cache_chunks,
                      sptr<backend> obj_backend);
    ~shared_read_cache();

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
    request *make_read_req(std::string img_prefix, uint64_t seqnum,
                           size_t obj_offset, size_t adjust, smartiov &dest);

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

/**
 * This is a request that is waiting on a backend request to complete.
 *
 * The trouble is that the backend request is pending at the time of request
 * creation* and the backend request might have completed by the time we get
 * around to running it.
 *
 * For now, if we detect that the backend is done when we run(), we just
 * directly call notify(). This *may* lead to a self-deadlock, but looks
 * like it's fine for now
 *
 * TODO fix this by moving dispatch to a separate thread in this case
 */
class shared_read_cache::pending_read_request : public self_refcount_request
{
  private:
    shared_read_cache &cache;
    chunk_idx chunk;
    size_t adjust;
    smartiov dest;

    request *parent = nullptr;

    /**
     * This mutex guards against the run and on_backend_done from being called
     * at the same time.
     */
    std::mutex mtx;
    bool is_backend_done = false;

  public:
    pending_read_request(shared_read_cache &cache, chunk_idx chunk,
                         size_t adjust, smartiov dest)
        : cache(cache), chunk(chunk), adjust(adjust), dest(dest)
    {
    }

    void try_notify()
    {
        if (is_backend_done && parent != nullptr) {
            parent->notify(this);
            this->dec_and_free();
        }
    }

    virtual void run(request *parent) override
    {
        std::unique_lock<std::mutex> l(mtx);
        assert(parent != nullptr);
        // assert(cache.get_chunk_status(chunk) != entry_status::VALID);

        this->parent = parent;
        try_notify();
    }

    void on_backend_done(void *buf)
    {
        std::unique_lock<std::mutex> l(mtx);
        dest.copy_in((char *)buf + adjust);
        is_backend_done = true;

        cache.dec_chunk_refcount(chunk);
        try_notify();
    }

    virtual void notify(request *child) override { UNIMPLEMENTED(); }
};

/**
 * Simple wrapper around the direct nvme request to decrement the refcount
 * to the cache chunk when the request is fully served
 */
class shared_read_cache::cache_hit_request : public self_refcount_request
{
  private:
    shared_read_cache &cache;
    chunk_idx chunk;
    request *store_req;
    request *parent = nullptr;

  public:
    cache_hit_request(shared_read_cache &cache, chunk_idx idx,
                      request *store_req)
        : cache(cache), chunk(idx), store_req(store_req)
    {
    }

    ~cache_hit_request() {}

    virtual void run(request *parent) override
    {
        assert(parent != nullptr);
        this->parent = parent;
        store_req->run(this);
    }

    virtual void notify(request *child) override
    {
        parent->notify(this);
        cache.dec_chunk_refcount(chunk);

        free_child(child);
        this->dec_and_free();
    }
};

/**
 * The cache miss path has multiple parts to it:
 *
 * - Allocate a chunk from the cache
 * - Allocate memory for the chunk for the backend request to write into
 * - Dispatch the backend end request, which writes into the memory buffer
 * - Once the backend request completes, we dispatch pending reads,
 *   notify the parent and we write the data into the cache
 * - Once the cache write completes, we notify the parent request and free
 *   the buffer
 *
 * Thus the in-memory buffer of the backend data only exists between the backend
 * request completing and the cache write completing.
 *
 * There is an alternative architecture where we just wait far the nvme cache
 * write to complete before dispatching everything, but that's additional
 * latency since that would now require two hops instead of one
 */
class shared_read_cache::cache_miss_request : public self_refcount_request
{
  private:
    shared_read_cache &cache;

    chunk_idx chunk;
    chunk_key key;
    void *buf;
    size_t adjust;
    smartiov dest;

    request *subrequest;
    bool is_backend_done = false;
    request *parent = nullptr;

  public:
    cache_miss_request(shared_read_cache &cache, chunk_idx chunk, chunk_key key,
                       void *buf, size_t adjust, smartiov dest,
                       request *subrequest)
        : cache(cache), chunk(chunk), key(key), buf(buf), adjust(adjust),
          dest(dest), subrequest(subrequest)
    {
    }

    ~cache_miss_request() {}

    virtual void run(request *parent)
    {
        assert(parent != nullptr);
        this->parent = parent;
        cache.set_chunk_status(chunk, entry_status::FETCHING);
        subrequest->run(this);
    }

    virtual void notify(request *child)
    {
        assert(child == subrequest);
        // dispatch depending on current state
        // the one who just completed can be either the backend or the nvme
        if (!is_backend_done)
            on_backend_done();
        else
            on_store_done();
    }

    void on_backend_done()
    {
        is_backend_done = true;

        std::vector<pending_read_request *> reqs;
        {
            std::unique_lock lock(cache.global_cache_lock);

            auto &entry = cache.cache_state[chunk];
            entry.status = entry_status::FILLING;
            entry.pending_fill_data = buf;

            reqs = entry.pending_reads;
            entry.pending_reads.clear();
        }

        for (auto &req : reqs)
            req->on_backend_done(buf);

        // there is enough info to complete the parent request, do it asap
        // to improve latency instead of waiting until the cache write is done
        dest.copy_in((char *)buf + adjust);
        parent->notify(this);
        free_child(subrequest);

        subrequest = cache.get_fill_req(chunk, buf);
        subrequest->run(this);
    }

    void on_store_done()
    {
        free_child(subrequest);
        cache.on_store_done(chunk);

        free(buf);
        this->dec_and_free();
    }
};

/**
 * Request to insert an object into the cache. Takes ownership of the passed-in
 * buffer and will free it on completion
 */
class shared_read_cache::cache_insert_request : public self_refcount_request
{
    shared_read_cache &cache;

    chunk_idx chunk;
    chunk_key key;
    void *buf;

    request *subrequest;

  public:
    cache_insert_request(shared_read_cache &cache, chunk_idx chunk,
                         chunk_key key, void *buf)
        : cache(cache), chunk(chunk), key(key), buf(buf)
    {
        subrequest = cache.cache_store->make_write_request(
            (char *)buf, CACHE_CHUNK_SIZE,
            cache.get_store_offset_for_chunk(chunk));

        // we shouldn't have a parent, so we only have 1 refcount
        dec_and_free();
    }

    void run(request *parent)
    {
        assert(parent == nullptr); // assume that we have no parent
        subrequest->run(this);
    }

    void notify(request *child)
    {
        assert(child == subrequest);
        free_child(subrequest);

        cache.on_store_done(chunk);
        free(buf);
        this->dec_and_free();
    }
};

shared_read_cache::shared_read_cache(std::string cache_path,
                                     size_t num_cache_blocks,
                                     sptr<backend> obj_backend)
    : cache_state(num_cache_blocks), size_in_chunks(num_cache_blocks),
      obj_backend(obj_backend),
      hitrate_stats(tag::rolling_window::window_size = CACHE_STATS_WINDOW),
      user_bytes(tag::rolling_window::window_size = CACHE_STATS_WINDOW),
      backend_bytes(tag::rolling_window::window_size = CACHE_STATS_WINDOW)
{
    trace("Opening {} for the read cache", cache_path);
    fd = open(cache_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0777);
    check_ret_errno(fd, "failed to open cache file");

    // think about persisting the cache state to disk so we can re-use it
    // on restart instead of filling it every time
    header_size_bytes = CACHE_HEADER_SIZE;

    cache_filesize_bytes =
        CACHE_CHUNK_SIZE * num_cache_blocks + CACHE_HEADER_SIZE;

    auto ret = ftruncate(fd, cache_filesize_bytes);
    check_ret_errno(ret, "failed to truncate cache file");

    // debug("Cache file size: {} KiB ({} header, {} chunks * {} per chunk)",
    //       cache_filesize_bytes / 1024, header_size_bytes, num_cache_blocks,
    //       CACHE_CHUNK_SIZE);

    cache_store = std::unique_ptr<nvme>(make_nvme_uring(fd, "rcache_uring"));
    cache_state = std::vector<entry_state>(num_cache_blocks);
}

shared_read_cache::~shared_read_cache() {}

chunk_idx shared_read_cache::allocate_chunk()
{
    // assumes that lock is held by the caller

    // search for the first empty or evictable entry
    [[maybe_unused]] int searched_entries = 0;
    while (true) {
        auto &entry = cache_state[clock_idx];

        if (entry.status == entry_status::EMPTY ||
            (entry.refcount == 0 && entry.accessed == false))
            break;

        // only clear abit if the entry is not in use
        if (entry.refcount == 0)
            entry.accessed = false;

        clock_idx = (clock_idx + 1) % size_in_chunks;
        searched_entries++;
    }

    // trace("allocate chunk: searched {} entries", searched_entries);

    // evict the entry
    auto &entry = cache_state[clock_idx];
    assert(entry.pending_fill_data == nullptr);
    assert(entry.pending_reads.empty());

    auto key = entry.key;
    cache_map.erase(key);

    entry.status = entry_status::EMPTY;

    auto ret = clock_idx;
    clock_idx = (clock_idx + 1) % size_in_chunks;
    return ret;
}

request *shared_read_cache::make_read_req(std::string img_prefix,
                                          uint64_t seqnum, size_t obj_offset,
                                          size_t adjust, smartiov &dest)
{
    auto req_size = dest.bytes();

    assert(obj_offset % CACHE_CHUNK_SIZE == 0);
    assert(adjust + req_size <= CACHE_CHUNK_SIZE);

    // trace("request: {} {} {} {}", img_prefix, seqnum, obj_offset, adjust);
    total_requests++;

    std::unique_lock lock(global_cache_lock);

    // Check if it's in the cache. If so, great, fetch it and return
    auto r = cache_map.find(std::make_tuple(img_prefix, seqnum, obj_offset));
    if (r != cache_map.end()) {
        auto idx = r->second;
        // trace("cache hit {}", idx);
        {
            std::lock_guard guard(cache_stats_lock);
            user_bytes(req_size);
            backend_bytes(0);
            hitrate_stats(1);
        }

        auto &entry = cache_state[idx];
        assert(entry.status != entry_status::EMPTY);

        // update cache metedata to pin in place and indicate access
        entry.refcount++;
        entry.accessed = true;

        // Happy path: the entry is valid, we fetch it and return
        if (entry.status == entry_status::VALID) {
            auto offset = get_store_offset_for_chunk(idx);
            auto req = cache_store->make_read_request(&dest, offset + adjust);
            return new cache_hit_request(*this, idx, req);
        }

        // If entry is FILLING:
        // Backend request is done but we're pushing it to nvme, in this case
        // the data will be in the pending_fill_data field and we should just
        // directly return it
        if (entry.status == entry_status::FILLING) {
            dest.copy_in((char *)entry.pending_fill_data + adjust);
            entry.refcount--;
            return nullptr;
        }

        // Backend request is pending, wait for it to complete
        if (entry.status == entry_status::FETCHING) {
            // trace("pending on chunk {}", idx);
            auto req = new pending_read_request(*this, idx, adjust, dest);
            entry.pending_reads.push_back(req);
            return req;
        }
    }

    // lock.unlock();

    // Cache miss path: allocate a chunk, fetch from backend, and then fill
    // the chunk in
    {
        std::lock_guard guard(cache_stats_lock);
        user_bytes(req_size);
        backend_bytes(CACHE_CHUNK_SIZE);
        hitrate_stats(0);
    }

    // upgrade to write lock
    // TODO verify safety? the only person to read the abit is the allocator,
    // and it is protected by an exclusive lock, so it should be fine
    auto buf = aligned_alloc(CACHE_CHUNK_SIZE, CACHE_CHUNK_SIZE);
    auto cache_key = std::make_tuple(img_prefix, seqnum, obj_offset);

    auto idx = allocate_chunk();
    auto &entry = cache_state[idx];
    entry.pending_fill_data = buf;
    entry.refcount++;
    entry.status = entry_status::FETCHING;
    entry.key = cache_key;

    objname oname(img_prefix, seqnum);
    auto backend_req =
        obj_backend->aio_read(oname.str(), obj_offset, buf, CACHE_CHUNK_SIZE);
    cache_map.insert(std::make_pair(cache_key, idx));
    auto req = new cache_miss_request(*this, idx, cache_key, buf, adjust, dest,
                                      backend_req);
    return req;
}

void shared_read_cache::insert_object(chunk_key key, void *data)
{
    std::unique_lock lock(global_cache_lock);

    auto idx = allocate_chunk();
    auto req = new cache_insert_request(*this, idx, key, data);

    auto &entry = cache_state[idx];
    entry.refcount++;
    entry.accessed = false; // clear abit so they're cleared the 1st time
    entry.status = entry_status::FILLING;
    entry.pending_fill_data = data;
    entry.key = key;

    cache_map.insert(std::make_pair(key, idx));

    lock.unlock();
    req->run(nullptr);
}

void shared_read_cache::on_store_done(chunk_idx chunk)
{
    std::unique_lock lock(global_cache_lock);

    auto &entry = cache_state[chunk];
    entry.status = entry_status::VALID;
    entry.pending_fill_data = nullptr;
    entry.refcount--;
}

bool shared_read_cache::should_bypass_cache()
{
    std::lock_guard lock(cache_stats_lock);

    auto frontend = rolling_sum(user_bytes);
    auto backend = rolling_sum(backend_bytes);
    double readamp = frontend == 0 ? 0 : (double)backend / (double)frontend;

    if (readamp > 2)
        return true;

    return false;
}

void shared_read_cache::served_bypass_request(size_t bytes)
{
    std::lock_guard lock(cache_stats_lock);
    total_requests++;
    hitrate_stats(0);
    user_bytes(bytes);
    backend_bytes(bytes);
}

void shared_read_cache::dec_chunk_refcount(chunk_idx chunk)
{
    std::unique_lock lock(global_cache_lock);
    auto &entry = cache_state[chunk];
    assert(entry.refcount > 0);
    entry.refcount--;
}

shared_read_cache::entry_status
shared_read_cache::get_chunk_status(chunk_idx chunk)
{
    std::unique_lock lock(global_cache_lock);
    return cache_state[chunk].status;
}

void shared_read_cache::set_chunk_status(chunk_idx chunk, entry_status status)
{
    std::unique_lock lock(global_cache_lock);
    cache_state[chunk].status = status;
}

size_t shared_read_cache::get_store_offset_for_chunk(chunk_idx chunk)
{
    return chunk * CACHE_CHUNK_SIZE + header_size_bytes;
}

request *shared_read_cache::get_fill_req(chunk_idx chunk, void *buf)
{
    auto offset = get_store_offset_for_chunk(chunk);
    return cache_store->make_write_request((char *)buf, CACHE_CHUNK_SIZE,
                                           offset);
}

class sharded_cache : public read_cache
{
    const size_t num_shards = 16;

    // TODO in-place obj instead of uptrs, don't understand why we can't have
    // just a vector of the plain object
    std::vector<uptr<shared_read_cache>> shards;

    // centralise the reporter
    std::thread cache_stats_reporter;
    std::atomic<bool> stop_cache_stats_reporter = false;

  public:
    sharded_cache(std::string cache_dir, size_t cache_bytes,
                  sptr<backend> obj_backend)
    {
        assert(cache_bytes % (num_shards * CACHE_CHUNK_SIZE) == 0);
        log_info(
            "Initialising read cache of size {} MiB in {} shards, {} MiB each",
            cache_bytes / 1024 / 1024, num_shards,
            cache_bytes / num_shards / 1024 / 1024);

        for (size_t i = 0; i < num_shards; i++) {
            auto shard_file =
                cache_dir + "/read_cache_shard_" + std::to_string(i);
            auto shard = std::make_unique<shared_read_cache>(
                shard_file, cache_bytes / num_shards / CACHE_CHUNK_SIZE,
                obj_backend);

            shards.push_back(std::move(shard));
        }

        cache_stats_reporter =
            std::thread(&sharded_cache::report_cache_stats, this);
    }

    ~sharded_cache() override
    {
        stop_cache_stats_reporter.store(true);
        cache_stats_reporter.join();
    }

    shared_read_cache &get_shard(chunk_key key)
    {
        auto shard_num = chunk_key_hash{}(key) % num_shards;
        return *shards.at(shard_num).get();
    }

    bool should_bypass_cache(std::string img_prefix, uint64_t seqnum,
                             size_t offset) override
    {
        auto key = std::make_tuple(img_prefix, seqnum, offset);
        // TODO aggregate stats from all the shards
        return get_shard(key).should_bypass_cache();
    }

    void served_bypass_request(std::string img_prefix, uint64_t seqnum,
                               size_t offset, size_t bytes) override
    {
        auto key = std::make_tuple(img_prefix, seqnum, offset);
        get_shard(key).served_bypass_request(bytes);
    }

    request *make_read_req(std::string img_prefix, uint64_t seqnum,
                           size_t offset, size_t adjust,
                           smartiov &dest) override
    {
        auto key = std::make_tuple(img_prefix, seqnum, offset);
        return get_shard(key).make_read_req(img_prefix, seqnum, offset, adjust,
                                            dest);
    }

    void insert_object(std::string img_prefix, uint64_t seqnum, size_t obj_size,
                       void *obj_data) override
    {
        // The incoming object is the raw object that's going to the backend; we
        // need to first chop it up into cache chunks before we can store it
        size_t processed_bytes = 0;
        size_t chunks = 0;
        while (processed_bytes < obj_size) {
            auto data = malloc(CACHE_CHUNK_SIZE);
            auto to_copy =
                std::min(obj_size - processed_bytes, CACHE_CHUNK_SIZE);
            memcpy(data, (char *)obj_data + processed_bytes, to_copy);

            auto key = std::make_tuple(img_prefix, seqnum, processed_bytes);
            get_shard(key).insert_object(key, data);

            processed_bytes += to_copy;
            chunks++;
        }

        trace("Inserting obj {}: {} bytes/{} chunks", seqnum, obj_size, chunks);
    }

    void report_cache_stats()
    {
        pthread_setname_np(pthread_self(), "rcache_stats");
        static size_t last_total_reqs = 0;

        auto wakes = 0;
        while (!stop_cache_stats_reporter.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1'000));
            if (wakes++ % 20 != 0)
                continue;

            size_t frontend = 0;
            size_t backend = 0;
            size_t hits = 0;
            size_t count = 0;
            size_t total_requests = 0;

            for (auto &shard : shards) {
                auto [user_bytes, backend_bytes, shard_hits, shard_count,
                      shard_total_requests] = shard->get_stats();

                frontend += user_bytes;
                backend += backend_bytes;
                hits += shard_hits;
                count += shard_count;
                total_requests += shard_total_requests;
            }

            // Don't report anything if there were no new requests
            if (total_requests == last_total_reqs)
                continue;

            last_total_reqs = total_requests;

            double readamp =
                frontend == 0 ? -1 : (double)backend / (double)frontend;

            debug("{}/{} hits, {} MiB read, {:.3f} read amp, {} total", hits,
                  count, frontend / 1024 / 1024, readamp, total_requests);
        }

        debug("cache stats reporter exiting");
    }
};

sptr<read_cache> get_read_cache_instance(std::string cache_dir,
                                         size_t cache_bytes,
                                         sptr<backend> obj_backend)
{
    auto static singleton = std::weak_ptr<read_cache>();
    if (auto ret = singleton.lock())
        return ret;

    auto new_instance =
        std::make_shared<sharded_cache>(cache_dir, cache_bytes, obj_backend);
    singleton = new_instance;
    return new_instance;
}
