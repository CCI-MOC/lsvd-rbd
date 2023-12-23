#include <atomic>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include "objname.h"
#include "shared_read_cache.h"

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
            cache.dec_chunk_refcount(chunk);
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
            std::unique_lock<std::shared_mutex> lock(cache.global_cache_lock);

            auto &entry = cache.cache_state[chunk];
            entry.status = entry_status::FILLING;
            // entry.pending_fill_data = buf;

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

        std::vector<pending_read_request *> reqs;
        {
            std::unique_lock<std::shared_mutex> lock(cache.global_cache_lock);

            auto &entry = cache.cache_state[chunk];
            entry.status = entry_status::VALID;
            cache.cache_map.left.insert(std::make_pair(key, chunk));

            // TEMPORARY HACK: disptach reads on store done because we don't
            // directly read from the buffer when it's there
            reqs = entry.pending_reads;
            entry.pending_reads.clear();

            // entry.pending_fill_data = nullptr;
            entry.refcount--;
        }

        for (auto &req : reqs)
            req->on_backend_done(buf);

        free(buf);
        this->dec_and_free();
    }
};

sptr<shared_read_cache>
shared_read_cache::get_instance(std::string cache_path, size_t num_cache_blocks,
                                sptr<backend> obj_backend)
{
    // If the last image is closed, clean up the cache as well. Don't keep the
    // cache around forever
    auto static singleton = std::weak_ptr<shared_read_cache>();

    auto instance = singleton.lock();
    if (sptr<shared_read_cache> ret = singleton.lock())
        return ret;

    auto new_instance = std::make_shared<shared_read_cache>(
        cache_path, num_cache_blocks, obj_backend);
    singleton = new_instance;
    return new_instance;
}

shared_read_cache::shared_read_cache(std::string cache_path,
                                     size_t num_cache_blocks,
                                     sptr<backend> obj_backend)
    : cache_state(num_cache_blocks), size_in_chunks(num_cache_blocks),
      obj_backend(obj_backend),
      hitrate_stats(tag::rolling_window::window_size = CACHE_STATS_WINDOW),
      user_bytes(tag::rolling_window::window_size = CACHE_STATS_WINDOW),
      backend_bytes(tag::rolling_window::window_size = CACHE_STATS_WINDOW)
{
    debug("Using {} as the shared read cache", cache_path);
    fd = open(cache_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0777);
    check_ret_errno(fd, "failed to open cache file");

    // think about persisting the cache state to disk so we can re-use it
    // on restart instead of filling it every time
    header_size_bytes = CACHE_HEADER_SIZE;

    cache_filesize_bytes =
        CACHE_CHUNK_SIZE * num_cache_blocks + CACHE_HEADER_SIZE;

    auto ret = ftruncate(fd, cache_filesize_bytes);
    check_ret_errno(ret, "failed to truncate cache file");

    debug("Cache file size: {} bytes ({} header, {} chunks * {} per chunk)",
          cache_filesize_bytes, header_size_bytes, num_cache_blocks,
          CACHE_CHUNK_SIZE);

    cache_store =
        std::unique_ptr<nvme>(make_nvme_uring(fd, "shared_read_cache"));
    cache_state = std::vector<entry_state>(num_cache_blocks);

    cache_stats_reporter =
        std::thread(&shared_read_cache::report_cache_stats, this);
}

shared_read_cache::~shared_read_cache()
{
    // TODO
    // flush all pending writes
    // free all buffers
    stop_cache_stats_reporter.store(true);
    cache_stats_reporter.join();
    close(fd);
}

chunk_idx shared_read_cache::allocate_chunk()
{
    // assumes that lock is held by the caller

    // search for the first empty or evictable entry
    int searched_entries = 0;
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
    entry.status = entry_status::EMPTY;
    cache_map.right.erase(clock_idx);

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

    std::shared_lock<std::shared_mutex> lock(global_cache_lock);

    // Check if it's in the cache. If so, great, fetch it and return
    auto r =
        cache_map.left.find(std::make_tuple(img_prefix, seqnum, obj_offset));
    if (r != cache_map.left.end()) {
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
        // TEMPORARY HACK: just use the pending read request and dispatch
        // the pending reads when the fill is done

        // Backend request is pending, wait for it to complete
        if (entry.status == entry_status::FETCHING ||
            entry.status == entry_status::FILLING) {
            trace("pending on chunk {}", idx);
            auto req = new pending_read_request(*this, idx, adjust, dest);
            entry.pending_reads.push_back(req);
            return req;
        }
    }

    lock.unlock();

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
    std::unique_lock<std::shared_mutex> ulock(global_cache_lock);

    auto idx = allocate_chunk();
    auto buf = aligned_alloc(CACHE_CHUNK_SIZE, CACHE_CHUNK_SIZE);

    auto &entry = cache_state[idx];
    // entry.pending_fill_data = buf;
    entry.refcount++;

    objname obj_name(img_prefix, seqnum);
    auto backend_req = obj_backend->make_read_req(
        obj_name.c_str(), obj_offset, (char *)buf, CACHE_CHUNK_SIZE);
    auto cache_key = std::make_tuple(img_prefix, seqnum, obj_offset);
    auto req = new cache_miss_request(*this, idx, cache_key, buf, adjust, dest,
                                      backend_req);
    return req;
}

void shared_read_cache::report_cache_stats()
{
    pthread_setname_np(pthread_self(), "cache_stats_reporter");
    static int last_total_reqs = 0;

    while (!stop_cache_stats_reporter.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20'000));
        std::lock_guard lock(cache_stats_lock);

        // Don't report anything if there were no new requests
        if (total_requests == last_total_reqs)
            continue;

        last_total_reqs = total_requests;

        auto frontend = rolling_sum(user_bytes);
        auto backend = rolling_sum(backend_bytes);
        double readamp =
            frontend == 0 ? -1 : (double)backend / (double)frontend;

        auto hits = rolling_sum(hitrate_stats);
        auto count = rolling_count(hitrate_stats);

        debug("{}/{} hits, {} MiB read, {:.3f} read amp, {} total", hits, count,
              frontend / 1024 / 1024, readamp, total_requests);
    }

    debug("cache stats reporter exiting");
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
    std::shared_lock<std::shared_mutex> lock(global_cache_lock);
    auto &entry = cache_state[chunk];
    assert(entry.refcount > 0);
    entry.refcount--;
}

shared_read_cache::entry_status
shared_read_cache::get_chunk_status(chunk_idx chunk)
{
    std::shared_lock<std::shared_mutex> lock(global_cache_lock);
    return cache_state[chunk].status;
}

void shared_read_cache::set_chunk_status(chunk_idx chunk, entry_status status)
{
    std::unique_lock<std::shared_mutex> lock(global_cache_lock);
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
