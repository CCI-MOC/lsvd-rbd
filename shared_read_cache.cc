#include <atomic>
#include <fcntl.h>
#include <unistd.h>

#include "objname.h"
#include "shared_read_cache.h"

/**
 * This is a workaround for the existing codebase doing ad-hoc lifetime
 * management for requests.
 *
 * From what I understand, the original codebase has requests that delete
 * themselves on `release()`, which the parent is reponsible for calling on
 * the child.
 *
 * But sometimes you have `release()` called before you can actually free
 * everything, so you need to do a self-reference count to prevent UAFs. The
 * self refcount is done in an ad-hoc manner with all sorts of state transitions
 * and it's a mess.
 *
 * The idea with this class is that each request really starts with two
 * referees: the completion event and the parent request. So what we do is we
 * start with refcount 2, and decrement on both release() and on notify().
 *
 * This is backwards compatible with the existing request structure; in an
 * ideal world, we would pass shared_from_this shared_ptrs to both the parent
 * and the child and they would decrement the refcount when they're done,
 * but that would require rewriting the lifetime management of all requests
 * in the entire codebase. This way is a drop-in replacement that has minimal
 * impact on everything else.
 *
 * Inpsired by the old read_cache::rcache_generic_request which had the same
 * idea but is badly implemented and a pain to use
 */
class self_refcount_request : public request
{
  protected:
    std::atomic_int refcount = 2;

    // TODO remove this; debugging double frees
    bool released = false;

    self_refcount_request() {}
    virtual ~self_refcount_request() {}

    void dec_and_free()
    {
        auto old = refcount.fetch_sub(1, std::memory_order_seq_cst);
        if (old == 1)
            delete this;
    }

    /**
     * Might move this into the destructor and lift subrequests into this class
     */
    void free_child(request *child)
    {
        if (child)
            child->release();
    }

  public:
    virtual void wait() override { UNIMPLEMENTED(); }
    virtual void release() override
    {
        if (released)
            throw std::runtime_error("double free");
        released = true;
        dec_and_free();
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
            cache.dec_chunk_refcount(chunk);
            this->dec_and_free();
        }
    }

    virtual void run(request *parent) override
    {
        std::unique_lock<std::mutex> l(mtx);
        assert(parent != nullptr);
        assert(cache.get_chunk_status(chunk) != entry_status::VALID);

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

        {
            std::unique_lock<std::shared_mutex> lock(cache.global_cache_lock);

            auto &entry = cache.cache_state[chunk];
            entry.status = entry_status::FILLING;
            // entry.pending_fill_data = buf;

            for (auto &req : entry.pending_reads)
                req->on_backend_done(buf);
            entry.pending_reads.clear();
        }

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

        {
            std::unique_lock<std::shared_mutex> lock(cache.global_cache_lock);

            auto &entry = cache.cache_state[chunk];
            entry.status = entry_status::VALID;
            cache.cache_map.left.insert(std::make_pair(key, chunk));

            // TEMPORARY HACK: disptach reads on store done because we don't
            // directly read from the buffer when it's there
            for (auto &req : entry.pending_reads)
                req->on_backend_done(buf);
            entry.pending_reads.clear();

            // entry.pending_fill_data = nullptr;
            free(buf);
            entry.refcount--;
        }

        this->dec_and_free();
    }
};

sptr<shared_read_cache>
shared_read_cache::get_instance(std::string cache_path, size_t num_cache_blocks,
                                sptr<backend> obj_backend)
{
    static sptr<shared_read_cache> instance = nullptr;
    if (instance == nullptr) {
        instance = std::make_shared<shared_read_cache>(
            cache_path, num_cache_blocks, obj_backend);
    }
    return instance;
}

shared_read_cache::shared_read_cache(std::string cache_path,
                                     size_t num_cache_blocks,
                                     sptr<backend> obj_backend)
    : cache_state(num_cache_blocks), size_in_chunks(num_cache_blocks),
      obj_backend(obj_backend)
{
    debug("Using {} as the shared read cache", cache_path);
    fd = open(cache_path.c_str(), O_RDWR | O_CREAT);
    check_ret(fd, "failed to open cache file");

    // think about persisting the cache state to disk so we can re-use it
    // on restart instead of filling it every time
    header_size_bytes = CACHE_HEADER_SIZE;

    cache_filesize_bytes =
        CACHE_CHUNK_SIZE * num_cache_blocks + CACHE_HEADER_SIZE;

    debug("Cache file size: {} bytes", cache_filesize_bytes);
    int ret = ftruncate(fd, cache_filesize_bytes);
    check_ret(ret, "failed to truncate cache file");

    cache_store =
        std::unique_ptr<nvme>(make_nvme_uring(fd, "shared_read_cache"));
    cache_state = std::vector<entry_state>(num_cache_blocks);
}

shared_read_cache::~shared_read_cache()
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

request *shared_read_cache::make_read_req(std::string img_prefix,
                                          uint64_t seqnum, size_t obj_offset,
                                          size_t adjust, smartiov &dest)
{
    assert(obj_offset % CACHE_CHUNK_SIZE == 0);
    assert(adjust + dest.bytes() <= CACHE_CHUNK_SIZE);

    trace("request: {} {} {} {}", img_prefix, seqnum, obj_offset, adjust);

    std::unique_lock<std::shared_mutex> lock(global_cache_lock);

    // Check if it's in the cache. If so, great, fetch it and return
    auto r =
        cache_map.left.find(std::make_tuple(img_prefix, seqnum, obj_offset));
    if (r != cache_map.left.end()) {
        auto idx = r->second;
        trace("cache hit {}", idx);

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
            auto req = new pending_read_request(*this, idx, adjust, dest);
            entry.pending_reads.push_back(req);
            return req;
        }
    }

    // Cache miss path: allocate a chunk, fetch from backend, and then fill
    // the chunk in
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
