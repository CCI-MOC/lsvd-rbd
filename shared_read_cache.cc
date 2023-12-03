#include <fcntl.h>
#include <unistd.h>

#include "objname.h"
#include "shared_read_cache.h"

/**
 * Simple wrapper around the direct nvme request to decrement the refcount
 * to the cache chunk when the request is fully served
 */
class shared_read_cache::cache_hit_request : public request
{
  private:
    uptr<request> store_req;
    chunk_idx chunk;
    sptr<shared_read_cache> cache;
    request *parent;

  public:
    cache_hit_request(uptr<request> store_req, chunk_idx idx,
                      sptr<shared_read_cache> cache)
    {
        this->store_req = std::move(store_req);
    }

    void run(request *parent) override
    {
        assert(parent != nullptr);
        this->parent = parent;
        store_req->run(this);
    }

    void notify(request *child) override
    {
        parent->notify(this);
        cache->dec_chunk_refcount(chunk);
    }

    void release() override { UNIMPLEMENTED(); }
    void wait() override { UNIMPLEMENTED(); }
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
class shared_read_cache::cache_miss_request : public request
{
  private:
    sptr<shared_read_cache> cache;

    chunk_idx chunk;
    void *buf;
    size_t adjust;
    smartiov dest;

    uptr<request> subrequest;
    bool is_backend_done = false;
    request *parent = nullptr;

  public:
    cache_miss_request(sptr<shared_read_cache> cache, chunk_idx chunk,
                       void *buf, size_t adjust, smartiov dest,
                       uptr<request> subrequest)
        : cache(cache), chunk(chunk), buf(buf), adjust(adjust), dest(dest),
          subrequest(std::move(subrequest))
    {
    }

    void run(request *parent)
    {
        assert(parent != nullptr);
        this->parent = parent;
        cache->set_chunk_status(chunk, entry_status::FETCHING);
        subrequest->run(this);
    }

    void on_backend_done()
    {
        is_backend_done = true;
        cache->set_chunk_status(chunk, entry_status::FILLING);
        cache->dispatch_pending_reads(chunk, buf);

        // there is enough info to complete the parent request, do it asap
        // to improve latency instead of waiting until the cache write is done
        dest.copy_in((char *)buf + adjust);
        parent->notify(this);

        subrequest = cache->get_fill_req(chunk, buf);
        subrequest->run(this);
    }

    void on_store_done()
    {
        cache->set_chunk_status(chunk, entry_status::VALID);
        cache->dec_chunk_refcount(chunk);
    }

    void notify(request *child)
    {
        // dispatch depending on current state
        // the one who just completed can be either the backend or the nvme
        if (!is_backend_done)
            on_backend_done();
        else
            on_store_done();
    }

    void release() override { UNIMPLEMENTED(); }
    void wait() override { UNIMPLEMENTED(); }
};

/**
 * This is a request that is waiting on a backend request to complete.
 *
 * The trouble is that the backend request is pending at the time of request
 * creation* and the backend request might have completed by the time we get
 * around to running it.
 *
 * In general, we also don't want to run the the notify in run because that'll
 * likely to self-deadlock. So the solution is to push the request to a
 * completion worker that handles the notify.
 *
 * TODO fix this
 */
class shared_read_cache::pending_read_request : public request
{
  private:
    chunk_idx chunk;
    sptr<shared_read_cache> cache;
    request *parent = nullptr;
    size_t adjust;
    smartiov dest;
    bool is_request_complete = false;

    std::mutex mtx;

  public:
    pending_read_request(chunk_idx chunk, sptr<shared_read_cache> cache,
                         size_t adjust, smartiov dest)
        : chunk(chunk), cache(cache), adjust(adjust), dest(dest)
    {
    }

    void run(request *parent) override
    {
        std::unique_lock<std::mutex> l(mtx);
        assert(parent != nullptr);
        assert(cache->get_chunk_status(chunk) != entry_status::VALID);

        this->parent = parent;
    }

    void on_backend_done(void *buf)
    {
        std::unique_lock<std::mutex> l(mtx);
        dest.copy_in((char *)buf + adjust);
        is_request_complete = true;
    }

    void on_complete()
    {
        parent->notify(this);
        cache->dec_chunk_refcount(chunk);
    }

  public:
    void notify(request *child) override { UNIMPLEMENTED(); }
    void release() override { UNIMPLEMENTED(); }
    void wait() override { UNIMPLEMENTED(); }
};

void shared_read_cache::dec_chunk_refcount(chunk_idx chunk)
{
    std::unique_lock<std::shared_mutex> lock(global_cache_lock);
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

void shared_read_cache::dispatch_pending_reads(chunk_idx chunk, void *buf)
{
    // Technically we only need a shared lock until we clear the pending reads
    // If this somehow becomes a bottleneck we can also just swap out the
    // vector with a lock, releasing it, and then iterating through it
    std::unique_lock<std::shared_mutex> lock(global_cache_lock);

    auto &entry = cache_state[chunk];
    for (auto &req : entry.pending_reads)
        req->on_backend_done(buf);
    entry.pending_reads.clear();
}

uptr<request> shared_read_cache::get_fill_req(chunk_idx chunk, void *buf)
{
    auto req = cache_store->make_write_request((char *)buf, CACHE_CHUNK_SIZE,
                                               chunk * CACHE_CHUNK_SIZE);
    return std::unique_ptr<request>(req);
}

sptr<shared_read_cache> get_instance(std::string cache_path,
                                     size_t num_cache_blocks,
                                     sptr<backend> obj_backend)
{
    // TODO make this a singleton
    return std::make_shared<shared_read_cache>(cache_path, num_cache_blocks,
                                               obj_backend);
}

shared_read_cache(std::string cache_path, size_t num_cache_blocks,
                  sptr<backend> obj_backend)
    : cache_state(num_cache_blocks), size_in_chunks(num_cache_blocks),
      obj_backend(obj_backend)
{
    fd = open(cache_path.c_str(), O_RDWR | O_CREAT, 0644);
    ftruncate(fd, CACHE_CHUNK_SIZE * num_cache_blocks);
    cache_store = std::make_unique<nvme>(fd);
}

~shared_read_cache()
{
    // TODO
    // flush all pending writes
    // free all buffers
    close(fd);
}

chunk_idx shared_read_cache::allocate_chunk()
{
    // assumes that lock is held by the caller

    // search for the first empty or evictable entry
    while (true) {
        auto &entry = cache_state[clock_idx];

        if (entry.status == entry_status::EMPTY ||
            (entry.refcount == 0 && entry.accessed == false))
            break;

        // only clear abit if the entry is not in use
        if (entry.refcount == 0)
            entry.accessed = false;

        clock_idx = (clock_idx + 1) % size_in_chunks;
    }

    // evict the entry
    auto &entry = cache_state[clock_idx];
    entry.status = entry_status::EMPTY;
    cache_map.right.erase(clock_idx);

    auto ret = clock_idx;
    clock_idx = (clock_idx + 1) % size_in_chunks;
    return ret;
}

std::shared_ptr<request>
shared_read_cache::make_read_req(std::string img_prefix, uint64_t seqnum,
                                 size_t obj_offset, size_t adjust,
                                 smartiov &dest)
{
    assert(obj_offset % CACHE_CHUNK_SIZE == 0);
    assert(adjust + dest.bytes() <= CACHE_CHUNK_SIZE);

    std::unique_lock<std::shared_mutex> lock(global_cache_lock);

    // Check if it's in the cache. If so, great, fetch it and return
    auto r =
        cache_map.left.find(std::make_tuple(img_prefix, seqnum, obj_offset));
    if (r != cache_map.left.end()) {
        auto idx = r->second;
        auto &entry = cache_state[idx];
        assert(entry.status != entry_status::EMPTY);

        // update cache metedata to pin in place and indicate access
        entry.refcount++;
        entry.accessed = true;

        // Happy path: the entry is valid, we fetch it and return
        if (entry.status == entry_status::VALID) {
            auto req = cache_store->make_read_request(
                &dest, idx * CACHE_CHUNK_SIZE + adjust);

            return std::make_unique<cache_hit_request>(
                std::unique_ptr<request>(req), idx, this);
        }

        // Backend request is pending, wait for it to complete
        if (entry.status == entry_status::FETCHING) {
            auto req =
                std::make_shared<pending_read_request>(idx, this, adjust, dest);
            entry.pending_reads.push_back(req);
            return req;
        }

        // Backend request is done but we're pushing it to nvme, in this case
        // the data will be in the pending_fill_data field and we should just
        // directly return it
        // TODO
    }

    // Cache miss path: allocate a chunk, fetch from backend, and then fill
    // the chunk in
    auto idx = allocate_chunk();
    auto buf = aligned_alloc(CACHE_CHUNK_SIZE, CACHE_CHUNK_SIZE);

    auto &entry = cache_state[idx];
    entry.pending_fill_data = buf;
    entry.refcount++;

    objname obj_name(img_prefix, seqnum);
    auto backend_req = obj_backend->make_read_req(
        obj_name.c_str(), obj_offset, (char *)buf, CACHE_CHUNK_SIZE);
    auto req = std::make_unique<cache_miss_request>(this, idx, buf, adjust,
                                                    dest, backend_req);
    return req;
}
