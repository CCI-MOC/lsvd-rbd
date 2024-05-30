#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <set>
#include <string.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <uuid/uuid.h>
#include <vector>
#include <zlib.h>

#include "extent.h"
#include "misc_cache.h"
#include "request.h"
#include "src/utils.h"
#include "translate.h"

/*
 * Architecture:
 *
 * all operations go through a single queue, handled by a single worker
 * thread. All sequence numbers are assigned by this thread, so operations
 * are totally ordered.
 *
 * - writes are aggregated into batches and submitted to the queue.
 *   objmap entries are updated before sending backend PUT request
 *
 * - checkpoint request: write the current map.
 *
 * - flush: block until all outstanding writes have completed
 *
 * - GC: the GC thread creates "tentative" GC writes, including all data
 *   to be written, and submits to the queue.
 *   Request processing (a) revalidates all data in the request,
 *   (b) updates the map, and (c) sends PUT request.
 *
 * in-memory map: the above steps leave the following windows where data
 * is not available for reading:
 *
 * 1. between (a1) appending to the current batch and (a2) assigning a
 *    sequence number - there's no "name" for the location of the data
 * 2. between (a2) assigning a sequence number and (a3) completion of
 *    PUT request - data may not be available for GET requests
 * 3. like 2, but for GC - after (b1) updating the map until (b2) the
 *    new PUT request is durable on the backend
 *
 * During each of these periods the data is guaranteed to be sitting in an
 * in-memory buffer. We keep a map from LBA to in-memory pointers, updating
 * it at points a1 and b1 and removing entries at a3 and b2 (i.e. write
 * request completion), being careful of overwrites in the interim.
 */

/* ----------- Object translation layer -------------- */

enum work_type { REQ_PUT = 1, REQ_FLUSH = 2, REQ_CKPT = 3, REQ_GC = 4 };

class translate_impl;

/* TODO: local_buf_base/limit are set in constructor for PUT,
 * translate_impl::write_gc for GC. this should be uniform...
 */

class translate_req : public request
{
    work_type op;
    translate_impl *tx;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    friend class translate_impl;

    /* REQ_PUT */
    char *batch_buf = NULL; // actual allocation
    char *data_ptr;         // batch_buf+room
    /* entries */

    size_t len = 0; // in bytes
    size_t max = 0;

    /* REQ_GC */
    char *gc_buf = NULL;  // object to write out
    char *gc_data = NULL; // passed in by GC thread

    /* lba/len/obj/offset (ignore obj/offset for REQ_PUT) */
    vec<ckpt_mapentry> entries;

    /* used for removing from map */
    char *local_buf_base = NULL;
    char *local_buf_limit = NULL;

    int _seq;

  public:
    char *append(int64_t lba, smartiov *iov)
    {
        assert(op == REQ_PUT);
        int64_t bytes = iov->bytes();
        entries.push_back((ckpt_mapentry){lba, bytes / 512, 0, 0});
        char *ptr = data_ptr + len;
        iov->copy_out(ptr);
        len += bytes;
        return ptr;
    }

    bool room(size_t bytes)
    {
        assert(op == REQ_PUT);
        return len + bytes <= max;
    }

    ~translate_req()
    {
        if (batch_buf)
            free(batch_buf);
    }

    /* NOTE - this assumes the only significant header entry is the map
     */
    translate_req(work_type op_, size_t bytes, translate_impl *tx_)
    {
        assert(op_ == REQ_PUT);
        op = op_;
        tx = tx_;
        int max_hdr_bytes = 1024 + (bytes / 2048) * sizeof(data_map);
        batch_buf = (char *)malloc(bytes + max_hdr_bytes);
        data_ptr = batch_buf + max_hdr_bytes;
        max = bytes;

        local_buf_base = data_ptr;
        local_buf_limit = data_ptr + bytes;
    }
    translate_req(work_type op_, translate_impl *tx_)
    {
        op = op_;
        tx = tx_;
    }

    void wait(void)
    {
        std::unique_lock lk(m);
        while (!done)
            cv.wait(lk);
        lk.unlock();
        delete this; // always call wait
    }

    void notify(request *child);
    void run(request *parent) {} // unused
    void release(void) {}        // unused
};

class translate_impl : public translate
{
    std::string name;
    lsvd_config &cfg;
    usize vol_size;

    std::shared_ptr<backend> objstore;
    std::shared_ptr<read_cache> rcache;

    // lock ordering: lock m before *map_lock
    std::mutex m; // for things in this instance
    std::condition_variable cv;

    extmap::objmap &objmap;      // shared object map
    std::shared_mutex &omap_mtx; // locks the object map
    extmap::bufmap &bufmap;      // shared object map
    std::mutex &bufmap_lock;

    std::atomic<seqnum_t> cur_seq;
    uint64_t ckpt_cache_seq = 0; // from last data object

    friend class translate_req;
    translate_req *current = NULL;

    vec<clone_base> &clones;
    std::map<seqnum_t, data_obj_info> &object_info;
    vec<seqnum_t> &checkpoints;

    std::atomic<u32> outstanding_writes = 0;

    // GC can't delete an object if the read logic has a
    // request outstanding to it - skip, and dead object reaping
    // will get it on the next pass.
    std::map<int, int> obj_read_refcount;

    // Used for updating the superblock when writing out new checkpoints
    // Reserve it up-front to avoid repeated allocations each time we serialise
    vec<byte> superblock_buf;

    thread_pool<translate_req *> *workers;

    opt<std::jthread> flush_worker;
    opt<std::jthread> gc_worker;

    // for triggering GC
    sector_t total_sectors = 0;
    sector_t total_live_sectors = 0;
    int gc_cycles = 0;
    int gc_sectors_read = 0;
    int gc_sectors_written = 0;
    int gc_deleted = 0;

    void write_checkpoint(seqnum_t seq, translate_req *req);
    void process_batch(seqnum_t seq, translate_req *req);
    void write_gc(seqnum_t _seq, translate_req *req);

    void worker_thread(thread_pool<translate_req *> *p);

    void make_obj_hdr(char *buf, uint32_t seq, sector_t hdr_sectors,
                      sector_t data_sectors, data_map *extents, int n_extents,
                      bool is_gc);

    void flush_thread(std::stop_token st);
    void gc_thread(std::stop_token st);
    void do_gc(std::stop_token &st);

  public:
    translate_impl(str name, lsvd_config &cfg, usize vol_size, uuid_t &vol_uuid,
                   sptr<backend> be, sptr<read_cache> rcache,
                   extmap::objmap &objmap, std::shared_mutex &omap_mtx,
                   extmap::bufmap &bmap, std::mutex &bmap_lck,
                   seqnum_t last_seq, vec<clone_base> &clones,
                   std::map<seqnum_t, data_obj_info> &objinfo,
                   vec<seqnum_t> &checkpoints)
        : translate(vol_uuid), name(name), cfg(cfg), vol_size(vol_size),
          objstore(be), rcache(rcache), objmap(objmap), omap_mtx(omap_mtx),
          bufmap(bmap), bufmap_lock(bmap_lck), cur_seq(last_seq + 1),
          clones(clones), object_info(objinfo), checkpoints(checkpoints),
          superblock_buf(4096)
    {
        // Calculate GC data
        for (auto const &[_, oi] : objinfo) {
            total_sectors += oi.data;
            total_live_sectors += oi.live;
        }

        current = new translate_req(REQ_PUT, cfg.backend_obj_size, this);
        assert(current->batch_buf != nullptr);

        // start worker, flush, and GC threads
        if (cfg.flush_interval_msec > 0)
            flush_worker =
                std::jthread([this](std::stop_token st) { flush_thread(st); });

        if (!cfg.no_gc)
            gc_worker =
                std::jthread([this](std::stop_token st) { gc_thread(st); });

        // honestly have no idea how this works
        workers = new thread_pool<translate_req *>(&m);
        workers->pool.push(
            std::thread(&translate_impl::worker_thread, this, workers));

        // Fully serialise superblock once, so we can do partial serialisations
        // later on and skip the checkpoint stuff every time
        // currently unimplemented
        // serialise_superblock(superblock_buf, checkpoints, clones, vol_uuid);
    }

    ~translate_impl()
    {
        cv.notify_all();
        if (workers)
            delete workers;
        if (current)
            delete current;
    }

    void flush(void) override;      /* write out current batch */
    void checkpoint(void) override; /* flush, then write checkpoint */

    ssize_t writev(uint64_t cache_seq, size_t offset, iovec *iov,
                   int iovcnt) override;
    ssize_t trim(size_t offset, size_t len) override;
    void backend_backpressure(void) override;

    // mark object as busy - can't delete
    void object_read_start(int obj) override;
    void object_read_end(int obj) override;

    void shutdown() override
    {
        if (gc_worker) {
            gc_worker->request_stop();
            gc_worker->join();
        }

        checkpoint();

        if (flush_worker) {
            flush_worker->request_stop();
            flush_worker->join();
        }
    }

    str prefix(seqnum_t seq) override
    {
        if (clones.size() == 0 || seq > clones.front().last_seq)
            return name;
        for (auto const &c : clones)
            if (seq >= c.first_seq)
                return c.name;
        assert(false); // unreachable
    }
};

uptr<translate> make_translate(str name, lsvd_config &cfg, usize vol_size,
                               uuid_t &vol_uuid, sptr<backend> be,
                               sptr<read_cache> rcache, extmap::objmap &objmap,
                               std::shared_mutex &omap_mtx,
                               extmap::bufmap &bmap, std::mutex &bmap_lck,
                               seqnum_t last_seq, vec<clone_base> &clones,
                               std::map<seqnum_t, data_obj_info> &objinfo,
                               vec<seqnum_t> &checkpoints)
{
    return std::unique_ptr<translate_impl>(new translate_impl(
        name, cfg, vol_size, vol_uuid, be, rcache, objmap, omap_mtx, bmap,
        bmap_lck, last_seq, clones, objinfo, checkpoints));
}

/* ----------- parsing and serializing various objects -------------*/

/* read object header
 *  fast: just read first 4KB
 *  !fast: read first 4KB, resize and read again if >4KB
 */

/* create header for a GC object
 */
void translate_impl::make_obj_hdr(char *buf, uint32_t _seq,
                                  sector_t hdr_sectors, sector_t data_sectors,
                                  data_map *extents, int n_extents, bool is_gc)
{
    auto h = (common_obj_hdr *)buf;
    auto dh = (obj_data_hdr *)(h + 1);
    uint32_t map_offset = sizeof(*h) + sizeof(*dh),
             map_len = n_extents * sizeof(data_map);
    uint32_t hdr_bytes = map_offset + map_len;
    assert(hdr_bytes <= hdr_sectors * 512);

    *h = (common_obj_hdr){.magic = LSVD_MAGIC,
                          .version = 1,
                          .vol_uuid = {0},
                          .type = OBJ_LOGDATA,
                          .seq = _seq,
                          .hdr_sectors = (uint32_t)hdr_sectors,
                          .data_sectors = (uint32_t)data_sectors,
                          .crc = 0};
    uuid_copy(h->vol_uuid, uuid);

    *dh = (obj_data_hdr){.cache_seq = 0,
                         .objs_cleaned_offset = 0,
                         .objs_cleaned_len = 0,
                         .data_map_offset = map_offset,
                         .data_map_len = map_len,
                         .is_gc = is_gc};

    data_map *dm = (data_map *)(dh + 1);
    for (int i = 0; i < n_extents; i++)
        *dm++ = extents[i];

    assert(hdr_bytes == ((char *)dm - buf));
    memset(buf + hdr_bytes, 0, 512 * hdr_sectors - hdr_bytes); // valgrind
    h->crc = (uint32_t)crc32(0, (const unsigned char *)buf, 512 * hdr_sectors);
}

/* ----------- data transfer logic -------------*/

/* NOTE: offset is in bytes
 */
ssize_t translate_impl::writev(uint64_t cache_seq, size_t offset, iovec *iov,
                               int iovcnt)
{

    smartiov siov(iov, iovcnt);
    size_t bytes = siov.bytes();
    sector_t base = offset / 512, limit = (offset + bytes) / 512;

    // maintain counter of writes
    static std::atomic_int counter = 0;
    static std::atomic_size_t user_write_bytes = 0;
    counter++;
    user_write_bytes += bytes;
    if (counter % 10'000 == 0)
        trace("{} write reqs, {} MiB total", counter,
              user_write_bytes / 1024 / 1024);

    // TODO lock granularity, we can just allocate space for the write here
    // and leave the actual memcpy to outside the lock
    // Break this apart into the critical allocation region and the non-critical
    // memcpy region, this preserves ordering between remote write log and
    // in-memory
    // To prevent a race condition on writing the object out before the memcpy
    // we need to pin it in memory, probably a refcount is sufficient.

    // roll-over in-memory log if necessary
    std::unique_lock lk(m);
    if (!current->room(bytes)) {
        workers->put_locked(current);
        current = new translate_req(REQ_PUT, cfg.backend_obj_size, this);
    }

    // write the data into the in-memory log
    auto ptr = current->append(base, &siov);

    // update the bufmap (lba -> in-memory buffer) with the extent
    std::unique_lock obj_w_lock(bufmap_lock);
    assert(ptr >= current->local_buf_base &&
           ptr + (limit - base) * 512 <= current->local_buf_limit);
    assert(ptr != NULL);
    bufmap.update(base, limit, ptr);

    return 0;
}

/* TRIM is not guaranteed durable, because:
 * 1. it doesn't remove data in the write pipeline
 * 2. it's not logged to the journal
 * 3. it's not logged in data objects
 * in other words, it's not persistent until the next checkpoint.
 *
 * to implement durable discard we would need to:
 * - add a tombstone record type to the write journal
 * - reserve a bit in struct data_map to indicate a trim with no data
 *
 * (note that we need to persist TRIM in order, as it can get overwritten)
 * TODO: do we need durable discard?
 */
ssize_t translate_impl::trim(size_t offset, size_t len)
{
    std::unique_lock lk(m);
    std::unique_lock obj_w_lock(omap_mtx);

    // trim the map
    vec<extmap::lba2obj> deleted;
    objmap.trim(offset / 512, (offset + len) / 512, &deleted);

    // and then update the GC accounting
    for (auto d : deleted) {
        auto [base, limit, ptr] = d.vals();
        if (object_info.find(ptr.obj) == object_info.end())
            continue; // skip clone base
        object_info[ptr.obj].live -= (limit - base);
        assert(object_info[ptr.obj].live >= 0);
        total_live_sectors -= (limit - base);
    }
    return 0;
}

/**
 * Backpressure for backend writes
 *
 * TODO measure how long this takes us, likely to be bottleneck on high
 * write throughput scenarios
 */
void translate_impl::backend_backpressure(void)
{
    std::unique_lock lk(m);
    while (outstanding_writes > cfg.num_parallel_writes)
        cv.wait(lk);
}

/* if there's an outstanding read on an object, we can't delete it in
 * garbage collection.
 */
void translate_impl::object_read_start(int obj)
{
    std::unique_lock lk(m);
    if (obj_read_refcount.find(obj) == obj_read_refcount.end())
        obj_read_refcount[obj] = 0;
    else
        obj_read_refcount[obj] = obj_read_refcount[obj] + 1;
}

void translate_impl::object_read_end(int obj)
{
    std::unique_lock lk(m);
    auto i = obj_read_refcount[obj];
    if (i == 1)
        obj_read_refcount.erase(obj);
    else
        obj_read_refcount[obj] = i - 1;
}

/* NOTE - currently not called for REQ_CKPT, which
 * uses sync write
 */
void translate_req::notify(request *child)
{
    if (child)
        child->release();

    if (op == REQ_PUT || op == REQ_GC) {
        /* wake up anyone waiting for TX window room
         * -> lock tx->m before tx->map_lock
         */
        std::unique_lock lk(tx->m);
        // if (--tx->outstanding_writes < tx->cfg.xlate_window)
        tx->outstanding_writes--;
        tx->cv.notify_all();
    }

    if (op == REQ_PUT) {
        /* remove extents from tx->bufmap, but only if they still
         * point to this buffer
         */
        std::unique_lock obj_w_lock(tx->bufmap_lock);
        vec<std::pair<sector_t, sector_t>> extents;
        for (auto const &e : entries) {
            auto limit = e.lba + e.len;
            for (auto it2 = tx->bufmap.lookup(e.lba);
                 it2 != tx->bufmap.end() && it2->base() < limit; it2++) {
                auto [_base, _limit, ptr] = it2->vals(e.lba, limit);
                if (ptr.buf >= local_buf_base && ptr.buf < local_buf_limit)
                    extents.push_back(std::pair(_base, _limit));
            }
        }
        for (auto [base, limit] : extents) {
            tx->bufmap.trim(base, limit);
        }
    }

    if (gc_buf != NULL) // allocated in write_gc
        free(gc_buf);
    if (gc_data != NULL) // allocated in gc threqad
        free(gc_data);

    if (op == REQ_GC) {
        std::unique_lock lk(m);
        done = true;
        cv.notify_one();
    } else
        delete this;
}

/* write out a checkpoint. Note that we don't have to lock the objmap,
 * since no one else is modifying it right now.
 *
 * possible changes:
 * - wait for preceding writes to complete before writing?
 * - write async rather than sync? (not really compatible with prev)
 */
void translate_impl::write_checkpoint(seqnum_t cp_seq, translate_req *req)
{
    debug("Writing checkpoint {}", cp_seq);

    vec<ckpt_mapentry> entries;
    vec<ckpt_obj> objects;

    for (auto it = objmap.begin(); it != objmap.end(); it++) {
        auto [base, limit, ptr] = it->vals();
        entries.push_back((ckpt_mapentry){.lba = base,
                                          .len = limit - base,
                                          .obj = (s32)ptr.obj,
                                          .offset = (s32)ptr.offset});
    }
    size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);

    for (auto it = object_info.begin(); it != object_info.end(); it++) {
        auto obj_num = it->first;
        auto [hdr, data, live] = it->second;
        objects.push_back((ckpt_obj){.seq = (uint32_t)obj_num,
                                     .hdr_sectors = (uint32_t)hdr,
                                     .data_sectors = (uint32_t)data,
                                     .live_sectors = (uint32_t)live});
    }
    size_t objs_bytes = objects.size() * sizeof(ckpt_obj);

    size_t hdr_bytes = sizeof(common_obj_hdr) + sizeof(obj_ckpt_hdr);
    sector_t sectors = div_round_up(hdr_bytes + map_bytes + objs_bytes, 512);

    vec<byte> cp_buf(sectors * 512);
    serialise_common_hdr(cp_buf, OBJ_CHECKPOINT, cp_seq, sectors, 0, uuid);
    auto ch = (obj_ckpt_hdr *)(cp_buf.data() + sizeof(common_obj_hdr));

    uint32_t o1 = sizeof(common_obj_hdr) + sizeof(obj_ckpt_hdr),
             o2 = o1 + objs_bytes;
    *ch = (obj_ckpt_hdr){.cache_seq = ckpt_cache_seq,
                         .ckpts_offset = 0,
                         .ckpts_len = 0,
                         .objs_offset = o1,
                         .objs_len = o2 - o1,
                         .deletes_offset = 0,
                         .deletes_len = 0,
                         .map_offset = o2,
                         .map_len = (uint32_t)map_bytes};

    auto objs = (char *)(ch + 1);
    auto maps = objs + objs_bytes;
    if (objs_bytes > 0)
        memcpy(objs, (char *)objects.data(), objs_bytes);
    if (map_bytes > 0)
        memcpy(maps, (char *)entries.data(), map_bytes);

    // Write out the checkpoint
    objstore->write(oname(name, cp_seq), cp_buf.data(), cp_buf.size());

    // Update superblock with new checkpoint, and keep only the last 3
    // around both in the backend and the superblock
    checkpoints.push_back(cp_seq);
    vec<seqnum_t> ckpts_to_delete;
    while (checkpoints.size() > 3) {
        ckpts_to_delete.push_back(checkpoints.front());
        checkpoints.erase(checkpoints.begin());
    }

    serialise_superblock(superblock_buf, checkpoints, clones, uuid, vol_size);
    // debug("Updating superblock with new checkpoint");
    objstore->write(name, superblock_buf.data(), superblock_buf.size());

    // debug("Deleting old checkpoints {}", ckpts_to_delete);
    for (auto c : ckpts_to_delete)
        objstore->delete_obj(oname(name, c));

    req->done = true;
    req->cv.notify_all();
}

/*
 * handle a "tentative" garbage collection request.
 *
 * the request has a list of LBA ranges, the object ranges the
 * ranges pointed to when they were selected for GC, and the data
 * those locations contained at that time.
 *
 * writes are not blocked while GC is building these requests, so
 * some ranges may have been overwritten.
 *
 * to process the request: (a) revalidate all mappings, discarding
 * any which have changed, and (b) assign a sequence number
 * this guarantees that the contents reflect the map state after all
 * previous seq#s and before all following ones.
 */
void translate_impl::write_gc(seqnum_t _seq, translate_req *req)
{
    req->_seq = _seq;

    int data_sectors = 0;
    for (const auto &e : req->entries)
        data_sectors += e.len;

    int max_hdr_bytes = sizeof(common_obj_hdr) + sizeof(obj_data_hdr) +
                        (cfg.backend_obj_size / 2048) * sizeof(data_map);
    int max_hdr_sectors = div_round_up(max_hdr_bytes, 512);

    auto buf = req->gc_buf =
        (char *)malloc((max_hdr_sectors + data_sectors) * 512);
    memset(buf, 0, max_hdr_sectors * 512);
    auto data_ptr = buf + max_hdr_sectors * 512;
    auto data_ptr0 = data_ptr;
    auto in_ptr = req->gc_data;

    // int _data_sectors = 0; // actual sectors in GC write
    vec<data_map> obj_extents;

    req->local_buf_base = data_ptr;
    for (auto const &[base, len, obj, offset] : req->entries) {
        auto limit = base + len;
        for (auto it2 = objmap.lookup(base);
             it2 != objmap.end() && it2->base() < limit; it2++) {
            /* [_base,_limit] is a piece of the extent
             * obj_base is where that piece starts in the object
             */
            auto [_base, _limit, ptr] = it2->vals(base, limit);
            if (ptr.obj != obj)
                continue;

            sector_t _sectors = _limit - _base;
            // _data_sectors += _sectors;
            int bytes = _sectors * 512;

            sector_t extent_offset = _base - base;
            memcpy(data_ptr, in_ptr + extent_offset * 512, bytes);
            data_ptr += bytes;
            obj_extents.push_back(
                (data_map){(uint64_t)_base, (uint64_t)(_limit - _base)});
        }
        in_ptr += len * 512;
    }
    req->local_buf_limit = data_ptr;
    data_sectors = (data_ptr - data_ptr0) / 512;

    int hdr_bytes = sizeof(common_obj_hdr) + sizeof(obj_data_hdr) +
                    obj_extents.size() * sizeof(data_map);
    int hdr_pages = div_round_up(hdr_bytes, 4096);
    int hdr_sectors = hdr_pages * 8;

    sector_t offset = hdr_sectors;
    data_ptr = data_ptr0;
    vec<extmap::lba2obj> deleted;
    req->entries.clear(); // replace with actual extents written

    std::unique_lock obj_w_lock(omap_mtx); // protect the readers
    for (auto const &e : obj_extents) {
        extmap::obj_offset oo = {_seq, offset};
        objmap.update(e.lba, e.lba + e.len, oo, &deleted);
        offset += e.len;
        req->entries.push_back(
            (ckpt_mapentry){(int64_t)e.lba, (int64_t)e.len, 0, 0});
    }
    obj_w_lock.unlock();

    for (auto &d : deleted) {
        auto [base, limit, ptr] = d.vals();
        if (object_info.find(ptr.obj) == object_info.end())
            continue; // skip clone base
        object_info[ptr.obj].live -= (limit - base);
        assert(object_info[ptr.obj].live >= 0);
        total_live_sectors -= (limit - base);
    }

    gc_sectors_written += data_sectors; // only written in this thread
    auto hdr = data_ptr0 - hdr_sectors * 512;
    make_obj_hdr(hdr, _seq, hdr_sectors, data_sectors, obj_extents.data(),
                 obj_extents.size(), true);

    auto h = (common_obj_hdr *)hdr;
    assert((int)h->hdr_sectors == hdr_sectors);

    data_obj_info oi = {
        .hdr = hdr_sectors, .data = data_sectors, .live = data_sectors};
    object_info[_seq] = oi;

    objname name(prefix(_seq), _seq);
    auto req2 = objstore->aio_write(name.str(), hdr,
                                    (hdr_sectors + data_sectors) * 512);
    outstanding_writes++;
    req2->run(req);
}

void translate_impl::process_batch(seqnum_t _seq, translate_req *req)
{
    req->_seq = _seq;

    int offset = sizeof(common_obj_hdr) + sizeof(obj_data_hdr),
        len = req->entries.size() * sizeof(data_map);
    int hdr_bytes = offset + len;
    int hdr_pages = div_round_up(hdr_bytes, 4096);
    int hdr_sectors = hdr_pages * 8;
    char *hdr_ptr = req->data_ptr - hdr_sectors * 512;
    int data_sectors = req->len / 512;

    /* update the object info table
     */
    std::unique_lock obj_w_lock(omap_mtx);

    object_info[_seq] = (data_obj_info){
        .hdr = hdr_sectors, .data = data_sectors, .live = data_sectors};

    /* and the object map (copy entries to right format at same time)
     */
    sector_t sector_offset = hdr_sectors;
    vec<extmap::lba2obj> deleted;
    deleted.reserve(req->entries.size());
    vec<data_map> dm_entries;
    dm_entries.reserve(req->entries.size());

    for (auto e : req->entries) {
        extmap::obj_offset oo = {_seq, sector_offset};
        objmap.update(e.lba, e.lba + e.len, oo, &deleted);
        sector_offset += e.len;
        dm_entries.push_back((data_map){(uint64_t)e.lba, (uint64_t)e.len});
    }

    for (auto d : deleted) {
        auto [base, limit, ptr] = d.vals();
        if (object_info.find(ptr.obj) == object_info.end())
            continue; // skip clone base
        object_info[ptr.obj].live -= (limit - base);
        assert(object_info[ptr.obj].live >= 0);
        total_live_sectors -= (limit - base);
    }

    obj_w_lock.unlock();

    /* update total and live sectors *after* dups are removed
     */
    auto live = object_info[_seq].live;
    total_live_sectors += live;
    total_sectors += data_sectors; // doesn't count headers

    make_obj_hdr(hdr_ptr, _seq, hdr_sectors, data_sectors, dm_entries.data(),
                 dm_entries.size(), false);

    auto pf = prefix(_seq);
    objname name(pf, _seq);
    auto obj_size = (hdr_sectors + data_sectors) * 512;
    auto obj_ptr = hdr_ptr;

    trace("Writing data obj seq {}", _seq);

    rcache->insert_object(pf, _seq, obj_size, obj_ptr);

    auto req2 = objstore->aio_write(name.str(), obj_ptr, obj_size);
    outstanding_writes++;
    req2->run(req);
}

void translate_impl::worker_thread(thread_pool<translate_req *> *p)
{
    pthread_setname_np(pthread_self(), "batch_worker");
    while (p->running) {
        std::unique_lock lk(m);
        translate_req *req;
        if (!p->get_locked(lk, req))
            return;

        /* note - flush operation has to put any partial batch on the
         * queue before queing a flush request
         */
        if (req->op == REQ_FLUSH) {
            while (outstanding_writes > 0)
                cv.wait(lk);
            req->done = true;
            req->cv.notify_all();
        }

        /* request and batch will be deleted on completion
         * map is updated before any following requests are processed
         */
        else if (req->op == REQ_PUT) {
            auto _seq = cur_seq++;
            lk.unlock();
            process_batch(_seq, req);
        }

        // generate a checkpoint before any following requests processed
        else if (req->op == REQ_CKPT) {
            auto _seq = cur_seq++;
            lk.unlock();
            write_checkpoint(_seq, req);
        }

        // handle output of GC thread
        else if (req->op == REQ_GC) {
            auto _seq = cur_seq++;
            lk.unlock();
            write_gc(_seq, req);
        }

        else
            assert(false);
    }
}

/* flushes any data buffered in current batch, and blocks until all
 * outstanding writes are complete.
 */
void translate_impl::flush(void)
{
    std::unique_lock lk(m);

    if (current->len > 0) {
        workers->put_locked(current);
        current = new translate_req(REQ_PUT, cfg.backend_obj_size, this);
    }

    auto flush_req = new translate_req(REQ_FLUSH, this);
    workers->put_locked(flush_req);
    lk.unlock();
    flush_req->wait();
}

void translate_impl::checkpoint(void)
{
    std::unique_lock lk(m);

    if (current->len > 0) {
        workers->put_locked(current);
        current = new translate_req(REQ_PUT, cfg.backend_obj_size, this);
    }

    auto ckpt_req = new translate_req(REQ_CKPT, this);
    workers->put_locked(ckpt_req);
    lk.unlock();
    ckpt_req->wait();
}

/* wake up every @wait_time, if data is pending in current batch
 * for @timeout then submit it for writing to the backend.
 * Unlike flush() we don't bother waiting until it completes.
 */
void translate_impl::flush_thread(std::stop_token st)
{
    pthread_setname_np(pthread_self(), "flush_thread");
    auto interval = std::chrono::milliseconds(cfg.flush_interval_msec);
    auto timeout = std::chrono::milliseconds(cfg.flush_timeout_msec);
    auto t0 = std::chrono::system_clock::now();
    auto seq0 = cur_seq.load();

    debug("Flush thread {} starting", pthread_self());

    while (true) {
        std::this_thread::sleep_for(interval);
        if (st.stop_requested())
            break;

        if (seq0 == cur_seq.load() && current->len > 0) {
            if (std::chrono::system_clock::now() - t0 < timeout)
                continue;
            workers->put_locked(current);
            current = new translate_req(REQ_PUT, cfg.backend_obj_size, this);
        } else {
            seq0 = cur_seq.load();
            t0 = std::chrono::system_clock::now();
        }
    }

    log_info("Flush thread {} exiting", pthread_self());
}

/* -------------- Garbage collection ---------------- */

struct _extent {
    int64_t base;
    int64_t limit;
    extmap::obj_offset ptr;
};

/* [describe GC algorithm here]
 */
void translate_impl::do_gc(std::stop_token &st)
{
    gc_cycles++;
    // trace("Start GC cycle {}", gc_cycles);
    int max_obj = cur_seq.load();

    std::shared_lock obj_r_lock(omap_mtx);
    vec<int> dead_objects;
    for (auto const &p : object_info) {
        auto [hdrlen, datalen, live] = p.second;
        if (live == 0) {
            total_sectors -= datalen;
            dead_objects.push_back(p.first);
        }
    }
    obj_r_lock.unlock();

    std::queue<request *> deletes;
    for (auto const &o : dead_objects) {
        /*
         * if there's an outstanding read on an object, we can't
         * delete it yet
         */
        {
            std::unique_lock lk(m);
            if (obj_read_refcount.find(o) != obj_read_refcount.end())
                continue;
        }
        objname name(prefix(o), o);
        auto r = objstore->aio_delete(name.str());
        r->run(NULL);
        deletes.push(r);
        while (deletes.size() > 8) {
            deletes.front()->wait();
            deletes.pop();
        }
    }
    while (deletes.size() > 0) {
        deletes.front()->wait();
        deletes.pop();
    }

    std::unique_lock obj_w_lock(omap_mtx);
    for (auto const &o : dead_objects)
        object_info.erase(o);
    obj_w_lock.unlock();

    std::unique_lock lk(m);
    auto last_ckpt =
        (checkpoints.size() > 0) ? checkpoints.back() : cur_seq.load();
    lk.unlock();

    /* create list of object info in increasing order of
     * utilization, i.e. (live data) / (total size)
     */
    obj_r_lock.lock();
    // int calculated_total = 0;
    std::set<std::tuple<double, int, int>> utilization;
    for (auto p : object_info) {
        if (p.first > last_ckpt)
            continue;
        auto [hdrlen, datalen, live] = p.second;
        double rho = 1.0 * live / datalen;
        sector_t sectors = hdrlen + datalen;
        // calculated_total += datalen;
        utilization.insert(std::make_tuple(rho, p.first, sectors));
    }
    obj_r_lock.unlock();

    /* gather list of objects needing cleaning, return if none
     */
    const double threshold = cfg.gc_threshold / 100.0;
    vec<std::pair<int, int>> objs_to_clean;
    for (auto [u, o, n] : utilization) {
        if (u > threshold)
            continue;
        if (objs_to_clean.size() > 32)
            break;
        objs_to_clean.push_back(std::make_pair(o, n));
    }
    if (objs_to_clean.size() == 0)
        return;

    /* find all live extents in objects listed in objs_to_clean:
     * - make bitmap from objs_to_clean
     * - find all entries in map pointing to those objects
     */
    std::set<int> objects;
    for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++)
        objects.insert(it->first);

    sector_t max_obj_sectors = 0;
    for (auto o : objects) {
        auto _sectors = object_info[o].hdr + object_info[o].data;
        max_obj_sectors = std::max(_sectors, max_obj_sectors);
    }

    obj_r_lock.lock();
    extmap::objmap live_extents;
    for (auto it = objmap.begin(); it != objmap.end(); it++) {
        auto [base, limit, ptr] = it->vals();
        if (ptr.obj <= max_obj && objects.find(ptr.obj) != objects.end())
            live_extents.update(base, limit, ptr);
        if (st.stop_requested()) // forced exit
            return;
    }
    obj_r_lock.unlock();

    /* Retrieve objects to clean, save to local file, build map
     * TODO:
     *  1. range read when appropriate
     *  2. get data from read cache when possible
     */
    if (live_extents.size() > 0) {
        /* temporary file, delete on close.
         */
        auto temp = fmt::format("{}/gc.XXXXXX", cfg.rcache_dir);
        auto t1 = strdup(temp.c_str());
        int fd = mkstemp(t1);
        free(t1);
        unlink(temp.c_str());

        /* read all objects in completely
         */
        std::map<int, int> file_map; // obj# -> sector offset in file
        sector_t offset = 0;
        char *buf = (char *)malloc(max_obj_sectors * 512);

        for (auto [i, sectors] : objs_to_clean) {
            objname name(prefix(i), i);
            objstore->read(name.str(), 0, buf, sectors * 512UL);
            gc_sectors_read += sectors;
            file_map[i] = offset;
            if (write(fd, buf, sectors * 512) < 0)
                throw("no space");
            offset += sectors;
            if (st.stop_requested())
                return;
        }
        free(buf);

        auto file_end = offset;

        vec<_extent> all_extents;
        for (auto it = live_extents.begin(); it != live_extents.end(); it++) {
            auto [base, limit, ptr] = it->vals();
            all_extents.push_back((_extent){base, limit, ptr});
        }

        /* outstanding writes
         */
        std::queue<translate_req *> requests;

        while (all_extents.size() > 0) {
            sector_t sectors = 0, max = cfg.backend_obj_size / 512;
            vec<_extent> extents;

            auto it = all_extents.begin();
            while (it != all_extents.end() && sectors < max) {
                extents.push_back(*it);
                sectors += (it->limit - it->base);
                it = all_extents.erase(it);
            }

            auto req = new translate_req(REQ_GC, this);
            auto file_data = req->gc_data = (char *)malloc(sectors * 512);
            size_t file_data_len = 0;

            for (auto [base, limit, ptr] : extents) {
                auto file_sector = file_map[ptr.obj] + ptr.offset;
                auto sectors = limit - base;
                assert(file_sector + sectors <= file_end);
                size_t bytes = sectors * 512;
                auto err = pread(fd, file_data + file_data_len, bytes,
                                 file_sector * 512);
                assert(err == (ssize_t)bytes);
                req->entries.push_back(
                    (ckpt_mapentry){.lba = base,
                                    .len = limit - base,
                                    .obj = (int)ptr.obj,
                                    .offset = (int)ptr.offset});
                file_data_len += bytes;
            }

            std::unique_lock lk(m);
            requests.push(req);
            workers->put_locked(req);
            lk.unlock();

            while ((int)requests.size() > cfg.gc_window &&
                   !st.stop_requested()) {
                auto t = requests.front();
                t->wait();
                requests.pop();
            }
            if (st.stop_requested())
                return;
        }

        while (requests.size() > 0 && !st.stop_requested()) {
            auto t = requests.front();
            t->wait();
            requests.pop();
        }
        close(fd);
        // unlink(temp);
    }

    /* defer deletes until we've cleaned the whole batch.
     */
    obj_w_lock.lock();
    for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++) {
        total_sectors -= object_info[it->first].data;
        object_info.erase(object_info.find(it->first));
    }
    obj_w_lock.unlock();

    if (st.stop_requested())
        return;

    /* write checkpoint *before* deleting any objects.
     * use single-threaded delete for now
     */
    if (objs_to_clean.size()) {
        checkpoint();
        for (auto it = objs_to_clean.begin(); it != objs_to_clean.end(); it++) {
            /*
             * if there's an outstanding read on an object, we can't
             * delete it yet
             */
            auto obj = it->first;
            {
                std::unique_lock lk(m);
                if (obj_read_refcount.find(obj) != obj_read_refcount.end())
                    continue;
            }
            objname name(prefix(obj), obj);
            objstore->delete_obj(name.str());
            gc_deleted++;
        }
    }
}

void translate_impl::gc_thread(std::stop_token st)
{
    debug("Starting GC");
    auto interval = std::chrono::milliseconds(200);
    // sector_t trigger = 128 * 1024 * 2; // 128 MB
    pthread_setname_np(pthread_self(), "gc_thread");

    while (!st.stop_requested()) {
        std::this_thread::sleep_for(interval);
        if (st.stop_requested())
            return;

        /* check to see if we should run a GC cycle
         */
        // if (total_sectors - total_live_sectors < trigger)
        //     continue;
        // if ((total_live_sectors / (double)total_sectors) > (cfg.gc_threshold
        // / 100.0)) continue;

        do_gc(st);
    }

    log_info("Stopping GC");
}
