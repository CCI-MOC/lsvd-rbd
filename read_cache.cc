/*
 * file:        img_reader.cc
 * description: implementation of read cache
 *              the read cache is:
 *                       * 1. indexed by obj/offset[*], not LBA
 *                       * 2. stores aligned 64KB blocks
 *                       * [*] offset is in units of 64KB blocks
 * author:      Peter Desnoyers, Northeastern University
 *              Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <algorithm> // std::min
#include <atomic>
#include <cassert>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <shared_mutex>
#include <stack>
#include <stdint.h>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <zlib.h> // DEBUG

#include "backend.h"
#include "lsvd_types.h"

#include "extent.h"
#include "misc_cache.h"
#include "smartiov.h"

#include "config.h"
#include "journal.h"
#include "nvme.h"
#include "request.h"
#include "translate.h"

#include "objname.h"
#include "read_cache.h"
#include "write_cache.h"

extern void do_log(const char *, ...);

/* rotating window statistics so we can forget the past...
 */
class window_ctr
{
    int i = 0;    // current epoch
    int epoch;    // number of sectors per epoch
    int n = 1000; // current # sectors
    int served[4] = {1000, 0, 0, 0};
    int fetched[4] = {0, 0, 0, 0};

  public:
    window_ctr(int epoch_) { epoch = epoch_; }
    ~window_ctr() {}

    void update(int srvd, int ftchd)
    {
        if (srvd + ftchd + n > epoch) {
            i++;
            served[i % 4] = 0;
            fetched[i % 4] = 0;
            n = 0;
        }
        served[i % 4] += srvd;
        fetched[i % 4] += ftchd;
        n += (srvd + ftchd);
    }
    void serve(int sectors) { update(sectors, 0); }
    void fetch(int sectors) { update(0, sectors); }
    std::pair<int, int> vals(void)
    {
        if (i < 1)
            return std::make_pair(served[0], fetched[0]);
        int i1 = (i + 3) % 4;
        int i2 = i % 4;
        return std::make_pair(served[i1] + served[i2],
                              fetched[i1] + fetched[i2]);
    }
};

class pending_read_request;

class reader_impl : public img_reader
{

    std::mutex m;
    int block_sectors;

    j_read_super *super;

    extmap::obj_offset *rmap;
    std::map<extmap::obj_offset, int> map;
    bool map_dirty = false;

    /* if map[obj,offset] = n:
     *   fetching[n] - backend read / nvme write not complete
     *   in_use[n] - not eligible for eviction
     *   written[n] - safe to read from cache
     *   pending[n] - continuations to invoke when buffer[n] becomes valid
     */
    std::vector<int> fetching;
    std::vector<int> in_use;
    std::vector<int> written;
    sized_vector<std::vector<pending_read_request *>> pending;

    // LBA -> objnum, sector_offset (on the backend)
    extmap::objmap *backend_objmap;
    std::shared_mutex *obj_lock;

    // LBA -> pointer to in-memory (pending writes)
    extmap::bufmap *pending_bufmap;
    std::mutex *bufmap_lock;

    translate *be;
    sptr<backend> io;
    nvme *ssd;
    lsvd_config *cfg;

    friend class cache_hit_request;
    friend class direct_read_req;
    friend class cache_fill_req;

    std::queue<int> free_blocks;
    std::condition_variable cv;

    window_ctr hit_stats;
    int pending_fills = 0; // max = cfg->fetch_window

    thread_pool<int> misc_threads; // eviction thread, for now

#if 0
    /* possible CLOCK implementation - queue holds <block,ojb/offset> 
     * pairs so that we can evict blocks without having to remove them 
     * from the CLOCK queue
     */
    sized_vector<char> a_bit;
    sized_vector<int>  block_version;
    std::queue<std::pair<int,extmap::obj_offset>> clock_queue;
#endif

    /* evict 'n' blocks - random replacement
     */
    void evict(int n);
    void evict_thread(thread_pool<int> *p);
    char *get_cacheline_buf(int n);

  public:
    reader_impl(uint32_t blkno, int _fd, translate *_be, lsvd_config *cfg,
                    extmap::objmap *map, extmap::bufmap *bufmap,
                    std::shared_mutex *m, std::mutex *bufmap_m,
                    sptr<backend> _io);

    ~reader_impl();

    void handle_read(size_t offset, smartiov *iovs,
                     std::vector<request *> &requests);

    sector_t nvme_sector(int blk)
    {
        auto val = (8 * super->base) + (blk * block_sectors);
        return val;
    }

    void write_map(void);
};

/* factory function so we can hide implementation
 */
img_reader *make_reader(uint32_t blkno, int _fd, translate *_be,
                            lsvd_config *cfg, extmap::objmap *map,
                            extmap::bufmap *bufmap, std::shared_mutex *m,
                            std::mutex *bufmap_m, sptr<backend> _io)
{
    return new reader_impl(blkno, _fd, _be, cfg, map, bufmap, m, bufmap_m,
                               _io);
}

/* constructor - allocate, read the superblock and map, start threads
 */
reader_impl::reader_impl(uint32_t blkno, int fd_, translate *be_,
                                 lsvd_config *cfg_, extmap::objmap *omap,
                                 extmap::bufmap *bmap,
                                 std::shared_mutex *maplock,
                                 std::mutex *bmap_lock, sptr<backend> io_)
    : hit_stats(5000000), misc_threads(&m)
{
    backend_objmap = omap;
    pending_bufmap = bmap;
    obj_lock = maplock;
    bufmap_lock = bmap_lock;
    be = be_;
    io = io_;
    cfg = cfg_;

    const char *name = "read_cache_cb";
    ssd = make_nvme_uring(fd_, name);

    char *buf = (char *)aligned_alloc(512, 4096);
    if (ssd->read(buf, 4096, 4096L * blkno) < 0)
        throw("read cache superblock");
    super = (j_read_super *)buf;

    block_sectors = super->unit_size;

    in_use.insert(in_use.end(), super->units, 0);
    written.insert(written.end(), super->units, false);
    fetching.insert(fetching.end(), super->units, false);
    pending.init(super->units);
    int rmap_bytes = round_up(super->units * sizeof(extmap::obj_offset), 4096);
    rmap = (extmap::obj_offset *)aligned_alloc(512, rmap_bytes);
    memset((char *)rmap, 0, rmap_bytes);

    auto val = ssd->read((char *)rmap, super->map_blocks * 4096,
                         super->map_start * 4096L);
    if (val < 0)
        throw("read flatmap");

    for (int i = 0; i < super->units; i++) {
        if (rmap[i].obj != 0) {
            map[rmap[i]] = i;
            written[i] = true;
        } else
            free_blocks.push(i);
    }

    map_dirty = false;

    misc_threads.pool.push(
        std::thread(&reader_impl::evict_thread, this, &misc_threads));
}

reader_impl::~reader_impl()
{
    misc_threads.stop(); // before we free anything threads might touch
    delete ssd;
    free((void *)super);
}

#if 0
static std::random_device rd; // to initialize RNG
static std::mt19937 rng(rd());
#else
static std::mt19937 rng(17); // for deterministic testing
#endif

/* evict 'n' blocks from cache, using random eviction
 * called with mutex *unlocked*
 */
void reader_impl::evict(int n)
{
    std::uniform_int_distribution<int> uni(0, super->units - 1);
    for (int i = 0; i < n; i++) {
        std::unique_lock lk(m);
        int j = uni(rng);
        while (rmap[j].obj == 0 || in_use[j] > 0)
            j = uni(rng);
        auto oo = rmap[j];
        rmap[j] = (extmap::obj_offset){0, 0};
        map.erase(oo);
        free_blocks.push(j);
    }
}

void reader_impl::evict_thread(thread_pool<int> *p)
{
    auto wait_time = std::chrono::milliseconds(500);
    auto t0 = std::chrono::system_clock::now();
    auto timeout = std::chrono::seconds(20);

    while (p->running) {
        std::unique_lock<std::mutex> lk(m);
        p->cv.wait_for(lk, wait_time);
        if (!p->running)
            return;

        int n = 0;
        if ((int)free_blocks.size() < super->units / 16)
            n = super->units / 4 - free_blocks.size();
        lk.unlock();

        if (n)
            evict(n);

        if (!map_dirty) // free list didn't change
            continue;

        /* write the map (a) immediately if we evict something, or
         * (b) occasionally if the map is dirty
         */
        auto t = std::chrono::system_clock::now();
        if (n > 0 || (t - t0) > timeout) {
            size_t bytes = 4096 * super->map_blocks;
            auto buf = (char *)aligned_alloc(512, bytes);
            memcpy(buf, (char *)rmap, bytes);

            if (ssd->write(buf, bytes, 4096L * super->map_start) < 0)
                throw("write flatmap");

            free(buf);
            t0 = t;
            map_dirty = false;
        }
    }
}

/* state machine for block obj,offset can be represented by the tuple:
 *  map=n - i.e. exists(n) | map[obj,offset] = n
 *  in_use[n] - 0 / >0
 *  written[n] - n/a, F, T
 *  pending[n] - n/a, [], [...]
 *
 * if not cached                          -> {!map, n/a}
 * first read will:
 *   - add to map
 *   - increment in_use
 *   - launch read                        -> {map=n, >0, F, []}
 * following reads will
 *   queue request to copy from buffer[n] -> {map=n, >0, F, [..]}
 * read complete will:
 *   - set buffer[n]
 *   - launch write                       -> {map=n, >0, F, [...]}
 * write complete will:
 *   - complete requests from pending[n]
 *   - set written[n] to true             -> {map=n, >0, T, []}
 * eviction of buffer will:
 *   - decr in_use                        -> {map=n, 0, T, []}
 * further reads will temporarily increment in_use
 * eviction will remove from map:         -> {!map, n/a}
 */

/* the fact that I'm doing this seems to indicate that the request
 * class needs some work...
 */
class rcache_generic_request : public request
{
    std::mutex m;
    bool done = false;
    bool released = false;

  public:
    rcache_generic_request() {}
    ~rcache_generic_request() {}
    void wait(void) {}

    void release(void)
    {
        std::unique_lock lk(m);
        released = true;
        if (done) {
            lk.unlock();
            delete this;
        }
    }

    void finish(void)
    {
        std::unique_lock lk(m);
        done = true;
        if (released) {
            lk.unlock();
            delete this;
        }
    }
};

/* instead of having a really complicated state machine, we
 * use separate request classes for the different types of
 * sub-requests
 */

/* pending read - cache hit, backend fetch is still in progress.
 * queue this and complete via copy_in()
 * NOTE - placed up here to get the damn thing to compile
 */
class pending_read_request : public rcache_generic_request
{
    reader_impl *r;
    sector_t sector_in_blk;
    smartiov iovs;
    request *parent;
    sector_t blk;
    bool was_run = false;
    bool done = false;
    std::mutex m;

  public:
    sector_t base, limit;
    pending_read_request(reader_impl *r_, sector_t blk_,
                         sector_t sector_in_blk_, smartiov &slice)
        : iovs(slice)
    {
        blk = blk_;
        sector_in_blk = sector_in_blk_;
    }

    ~pending_read_request() {}
    void notify(request *r) {}

    /* weird race condition - we might actually get completed before
     * the parent has called run()
     */
    void run(request *parent_)
    {
        std::unique_lock lk(m);
        parent = parent_;
        was_run = true;
        if (done) {
            lk.unlock();
            parent->notify(this);
            finish();
        }
    }

    void copy_in(char *buf)
    {
        iovs.copy_in(buf + sector_in_blk * 512L);
        std::unique_lock lk(m);
        done = true;
        if (was_run) {
            lk.unlock();
            parent->notify(this);
            finish();
        }
    }
};

/* cache hit - fetch from NVMe
 */
class cache_hit_request : public rcache_generic_request
{
    reader_impl *r;
    smartiov iovs;
    int blk;

    request *parent;
    request *nvme_req;

  public:
    cache_hit_request(reader_impl *r_, int blk_, sector_t nvme_sector,
                      smartiov &slice)
        : iovs(slice)
    {
        r = r_;
        blk = blk_;
        nvme_req = r->ssd->make_read_request(&iovs, nvme_sector * 512L);
    }
    ~cache_hit_request() {}

    void run(request *parent_)
    {
        parent = parent_;
        nvme_req->run(this);
    }

    void notify(request *child)
    {
        if (child)
            child->release();
        std::unique_lock lk(r->m);
        r->in_use[blk]--; // CACHE HIT IN_USE DEC
        parent->notify(this);
        finish();
    }
};

/* cache bypass - read directly from backend object
 * (for thrash avoidance)
 */
class direct_read_req : public rcache_generic_request
{
    smartiov iovs;

    request *obj_req = NULL;
    request *parent = NULL;
    reader_impl *r = NULL;
    extmap::obj_offset oo;

    std::mutex m;
    bool done = false;
    bool release = false;

  public:
    direct_read_req(reader_impl *r_, extmap::obj_offset oo_,
                    smartiov &slice)
        : iovs(slice)
    {
        oo = oo_;
        r = r_;
        objname name(r->be->prefix(oo.obj), oo.obj);
        auto [iov, iovcnt] = iovs.c_iov();
        obj_req =
            r->io->make_read_req(name.c_str(), 512L * oo.offset, iov, iovcnt);
    }
    ~direct_read_req() {}

    void run(request *parent_)
    {
        parent = parent_;
        r->be->object_read_start(oo.obj);
        obj_req->run(this);
    }

    void notify(request *child)
    {
        if (child)
            child->release();
        r->be->object_read_end(oo.obj);
        parent->notify(this);
        finish();
    }
};

class cache_fill_req : public rcache_generic_request
{
    reader_impl *r;
    int blk = 10000000;
    sector_t sector_in_blk; // specific request
    extmap::obj_offset oo;  // block location in object
    sector_t nvme_sector;
    smartiov iovs;
    request *parent;

    request *nvme_req;
    request *obj_req;

    char *buf;

    enum { FETCH_PENDING = 1, WRITE_PENDING } state;

    uint32_t crc1; // DEBUG
  public:
    cache_fill_req(reader_impl *r_, int blk_, sector_t sector_in_blk_,
                   extmap::obj_offset obj_loc_, sector_t nvme_sector_,
                   smartiov &slice)
        : iovs(slice)
    {
        r = r_;
        blk = blk_;
        sector_in_blk = sector_in_blk_;
        oo = obj_loc_;
        nvme_sector = nvme_sector_;
        buf = (char *)aligned_alloc(512, r->block_sectors * 512);
        memset(buf, 'A', 64 * 1024);

        objname name(r->be->prefix(oo.obj), oo.obj);
        obj_req = r->io->make_read_req(name.c_str(), 512L * oo.offset, buf,
                                       r->block_sectors * 512);
    }

    ~cache_fill_req() { free(buf); }

    void run(request *parent_)
    {
        parent = parent_;
        state = FETCH_PENDING;
        r->be->object_read_start(oo.obj);
        obj_req->run(this);
    }

    void notify(request *child)
    {
        if (child)
            child->release();
        if (state == FETCH_PENDING) {
            r->be->object_read_end(oo.obj);

            // /* complete any higher-layer requests waiting on this
            //  */
            // std::unique_lock lk(r->m);
            // for (auto const &req : r->pending[blk])
            // 	req->copy_in(buf);
            // r->pending[blk].clear();
            // lk.unlock();

            // iovs.copy_in(buf + sector_in_blk * 512L);
            // parent->notify(this);

            /* and write it to cache
             */
            size_t len = r->block_sectors * 512;
            crc1 = (uint32_t)crc32(0, (unsigned char *)buf, len);
            nvme_req = r->ssd->make_write_request(buf, len, nvme_sector * 512L);
            state = WRITE_PENDING;
            nvme_req->run(this);
        } else if (state == WRITE_PENDING) {
            size_t len = r->block_sectors * 512;
            auto crc2 = (uint32_t)crc32(0, (unsigned char *)buf, len);
            assert(crc1 == crc2);
            std::unique_lock lk(r->m);
            iovs.copy_in(buf + sector_in_blk * 512L);
            parent->notify(this);
            for (auto req : r->pending[blk])
                req->copy_in(buf);
            r->pending[blk].clear();

            r->in_use[blk]--; // CACHE FILL IN_USE DECR
            r->fetching[blk] = false;
            r->pending_fills--;
            r->cv.notify_all();
            lk.unlock();

            finish();
        }
    }
};

void reader_impl::handle_read(size_t offset, smartiov *iovs,
                                  std::vector<request *> &requests)
{
    sector_t start_sector = offset / 512;
    sector_t end_sector = start_sector + iovs->bytes() / 512;

    std::unique_lock lk(m);
    std::shared_lock lk2(*obj_lock);
    std::unique_lock lk3(*bufmap_lock);

    auto bufmap_it = pending_bufmap->end();
    if (pending_bufmap->size() > 0)
        bufmap_it = pending_bufmap->lookup(start_sector);
    auto backend_it = backend_objmap->lookup(start_sector);
    size_t _offset = 0;

    /*
     *       base                         limit
     *       |------------------------------|
     * +------------------------------+-------------------- ...
     *       ^base                    ^limit1       <- iter 1
     *                                ^base ^limit1 <- iter 2
     * map1              +---+
     *       ^base       ^limit2                    <- iter 1.1
     *                       ^base    ^limit2       <- iter 1.2
     * map2  +----+   +----+ ...                    <- iter 1.1.*
     *
     * - We start with a contiguous read in LBA space. This read may be
     *   fragmented across the backend, with one extent in pending writes,
     *   another in the backend, another unmapped, etc
     * - We start at the beginning of the read in LBA space, and march forward,
     *   looking for the location of the next extent in each of the maps.
     * - The pending writes map has the highest priority
     * - Then it goes to the backend
     * - If it's unmapped, then we just zero the output buffer and return
     */
    while (start_sector < end_sector) {

        // first we look in the pending writes map

        /* ugly hack because end()->vals() crashes
         */
        sector_t base1 = end_sector, limit1 = end_sector;
        extmap::sector_ptr bufptr = {NULL};
        if (bufmap_it != pending_bufmap->end())
            std::tie(base1, limit1, bufptr) =
                bufmap_it->vals(start_sector, end_sector);

        sector_t base2 = end_sector, limit2 = end_sector;
        extmap::obj_offset objptr = {0, 0};
        if (backend_it != backend_objmap->end())
            std::tie(base2, limit2, objptr) =
                backend_it->vals(start_sector, end_sector);

        /* [base..base3] is unmapped - zero it out
         */
        auto base3 = std::min(base1, base2);
        if (base3 > start_sector) {
            sector_t sectors = base3 - start_sector;
            iovs->zero(_offset, _offset + sectors * 512L);
            start_sector = base3;
            _offset += sectors * 512L;
            continue;
        }

        /* buffer map (i.e. current batch buffer) takes priority
         */
        if (base1 == start_sector) {
            sector_t sectors = limit1 - start_sector;
            auto slice = iovs->slice(_offset, _offset + sectors * 512L);
            slice.copy_in(bufptr.buf);
            hit_stats.serve(sectors);

            start_sector = limit1;
            _offset += sectors * 512;

            /* skip any objmap extents fully obscured by this extent:
             *  it1:  |---------------------|
             *  it2:    |----|    |----|
             *                           |------|   < but not this
             */
            while (backend_it != backend_objmap->end() && backend_it->limit() <= limit1)
                backend_it++;
            bufmap_it++;
            continue;
        }

        assert(base2 == start_sector);
        assert(backend_it != backend_objmap->end());
        limit2 = std::min(limit2, base1);
        sector_t sectors = limit2 - start_sector;

        /* objects are cached in aligned blocks of size <block_sectors>
         */
        extmap::obj_offset key = {objptr.obj,
                                  round_down(objptr.offset, block_sectors)};
        sector_t offset_limit = std::min(key.offset + block_sectors,
                                         (int)(objptr.offset + sectors));

        /* if we find the data in cache, fetch it
         */
        if (map.find(key) != map.end()) {
            sector_t sectors = (offset_limit - objptr.offset);
            sector_t sector_in_blk = objptr.offset % block_sectors;
            limit2 = start_sector + sectors;

            hit_stats.serve(sectors);
            auto slice = iovs->slice(_offset, _offset + sectors * 512L);
            int i = map[key];

            if (fetching[i]) {
                auto req =
                    new pending_read_request(this, i, sector_in_blk, slice);
                req->base = start_sector;
                req->limit = limit2;
                pending[i].push_back(req);
                requests.push_back(req);
            } else {
                in_use[i]++; // CACHE HIT IN_USE INCR
                auto nvme_location = nvme_sector(i) + sector_in_blk;
                auto req = new cache_hit_request(this, i, nvme_location, slice);
                requests.push_back(req);
            }
            _offset += (limit2 - start_sector) * 512;
            start_sector = limit2;
            if (limit2 == backend_it->limit())
                backend_it++;
            continue;
        }

        /* Thrash prevention factors:
         * - fetch efficiency (user vs backend bytes)
         * - window on NVMe writes for cache fill
         *   (or should it be window on cache fills?)
         * - available cache blocks
         * TODO: don't let hits get too high
         */
        auto [served, fetched] = hit_stats.vals();
        bool use_cache = (free_blocks.size() > 0) &&
                         (served > fetched * (cfg->fetch_ratio / 100.0)) &&
                         (pending_fills < cfg->fetch_window);

        /* if we bypass the cache, send the entire read to the backend
         * without worrying about cache block alignment
         */
        if (!use_cache) {
            sector_t sectors = limit2 - start_sector;
            auto slice = iovs->slice(_offset, _offset + sectors * 512L);
            auto req = new direct_read_req(this, objptr, slice);
            requests.push_back(req);

            hit_stats.serve(sectors);
            hit_stats.fetch(sectors);

            _offset += sectors * 512L;
            start_sector = limit2;
            if (limit2 == backend_it->limit())
                backend_it++;
            continue;
        }

        /* standard cache miss path - fetch and insert
         */
        else {
            int i = free_blocks.front();
            free_blocks.pop();

            in_use[i]++; // CACHE FILL IN_USE INCR
            fetching[i] = true;
            map[key] = i;
            rmap[i] = key;

            sector_t sectors = offset_limit - objptr.offset;
            limit2 = start_sector + sectors;
            auto slice = iovs->slice(_offset, _offset + sectors * 512L);
            sector_t sector_in_blk = objptr.offset % block_sectors;

            /* parameters to the request:
             * - lba range of this request
             * - object number/name
             * - range in object corresponding to cache block
             * - cache block number to store it into
             * - offset in cache block
             * - iovec etc.
             */
            auto nvme_base = nvme_sector(i);
            auto req = new cache_fill_req(this, i, sector_in_blk, key,
                                          nvme_base, slice);
            requests.push_back(req);

            hit_stats.serve(sectors);
            hit_stats.fetch(block_sectors);
            pending_fills++;

            _offset += (limit2 - start_sector) * 512;
            start_sector = limit2;
            if (limit2 == backend_it->limit())
                backend_it++;
        }
    }
}

void reader_impl::write_map(void)
{
    if (ssd->write((char *)rmap, 4096 * super->map_blocks,
                   4096L * super->map_start) < 0)
        throw("write flatmap");
}
