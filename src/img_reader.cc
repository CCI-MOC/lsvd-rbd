#include <algorithm> // std::min
#include <cassert>
#include <mutex>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <zlib.h> // DEBUG

#include "backend.h"
#include "config.h"
#include "extent.h"
#include "img_reader.h"
#include "lsvd_types.h"
#include "request.h"
#include "shared_read_cache.h"
#include "smartiov.h"
#include "translate.h"

class reader_impl : public img_reader
{
    std::mutex m;
    int block_sectors = CACHE_CHUNK_SIZE / 512;

    // LBA -> objnum, sector_offset (on the backend)
    extmap::objmap *backend_objmap;
    std::shared_mutex *obj_lock;

    // LBA -> pointer to in-memory (pending writes)
    extmap::bufmap *pending_bufmap;
    std::mutex *bufmap_lock;

    translate *be;
    sptr<backend> io;
    lsvd_config *cfg;

    sptr<read_cache> backing_cache;

    friend class direct_read_req;

  public:
    reader_impl(uint32_t blkno, translate *be_, lsvd_config *cfg_,
                extmap::objmap *omap, extmap::bufmap *bmap,
                std::shared_mutex *maplock, std::mutex *bmap_lock,
                sptr<backend> io_, sptr<read_cache> backing_cache)
        : backing_cache(backing_cache)
    {
        backend_objmap = omap;
        pending_bufmap = bmap;
        obj_lock = maplock;
        bufmap_lock = bmap_lock;
        be = be_;
        io = io_;
        cfg = cfg_;
    }

    ~reader_impl() {}

    void handle_read(size_t offset, smartiov *iovs,
                     std::vector<request *> &requests);
};

/* factory function so we can hide implementation
 */
uptr<img_reader> make_reader(uint32_t blkno, translate *_be, lsvd_config *cfg,
                             extmap::objmap *map, extmap::bufmap *bufmap,
                             std::shared_mutex *m, std::mutex *bufmap_m,
                             sptr<backend> _io, sptr<read_cache> backing_cache)
{
    return std::make_unique<reader_impl>(blkno, _be, cfg, map, bufmap, m,
                                         bufmap_m, _io, backing_cache);
}

/* cache bypass - read directly from backend object
 * (for thrash avoidance)
 */
class direct_read_req : public self_refcount_request
{
    smartiov iovs;

    request *obj_req = NULL;
    request *parent = NULL;
    reader_impl *r = NULL;
    extmap::obj_offset oo;

  public:
    direct_read_req(reader_impl *r_, extmap::obj_offset oo_, smartiov &slice)
        : iovs(slice)
    {
        oo = oo_;
        r = r_;
        objname name(r->be->prefix(oo.obj), oo.obj);
        obj_req = r->io->aio_read(name.str(), 512L * oo.offset, iovs);
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
        free_child(child);
        r->be->object_read_end(oo.obj);
        parent->notify(this);
        dec_and_free();
    }
};

void reader_impl::handle_read(size_t offset, smartiov *iovs,
                              std::vector<request *> &requests)
{
    sector_t start_sector = offset / 512;
    sector_t end_sector = start_sector + iovs->bytes() / 512;

    /**
     * These locks are too coarse, they block everything
     */
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
            // hit_stats.serve(sectors);

            start_sector = limit1;
            _offset += sectors * 512;

            /* skip any objmap extents fully obscured by this extent:
             *  it1:  |---------------------|
             *  it2:    |----|    |----|
             *                           |------|   < but not this
             */
            while (backend_it != backend_objmap->end() &&
                   backend_it->limit() <= limit1)
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

        /* Thrash prevention factors:
         * - fetch efficiency (user vs backend bytes)
         * - window on NVMe writes for cache fill
         *   (or should it be window on cache fills?)
         * - available cache blocks
         * TODO: don't let hits get too high
         */
        // auto [served, fetched] = hit_stats.vals();
        // bool use_cache = (free_blocks.size() > 0) &&
        //                  (served > fetched * (cfg->fetch_ratio / 100.0)) &&
        //                  (pending_fills < cfg->fetch_window);

        /* if we bypass the cache, send the entire read to the backend
         * without worrying about cache block alignment
         */
        // if (backing_cache->should_bypass_cache()) {
        if (false) {
            sector_t sectors = limit2 - start_sector;
            // backing_cache->served_bypass_request(sectors * 512L);
            auto slice = iovs->slice(_offset, _offset + sectors * 512L);
            auto req = new direct_read_req(this, objptr, slice);
            requests.push_back(req);

            // hit_stats.serve(sectors);
            // hit_stats.fetch(sectors);

            _offset += sectors * 512L;
            start_sector = limit2;
            if (limit2 == backend_it->limit())
                backend_it++;
            continue;
        }

        // standard cache miss path - fetch and insert
        // This file was formerly the image-specific read_cache, but I
        // implemented the read_cache by ripping out all the caching
        // portions of this class and just pretending like the cache is the
        // backend. What this does here is just pretend like the cache is the
        // backend and the cache will handle the rest
        else {
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
            // auto nvme_base = nvme_sector(i);
            // auto req = new cache_fill_req(this, i, sector_in_blk, key,
            //                               nvme_base, slice);

            // same thing to the shared read cache
            auto prefix = be->prefix(key.obj);
            auto req =
                backing_cache->make_read_req(prefix, key.obj, key.offset * 512L,
                                             sector_in_blk * 512L, slice);
            if (req != nullptr)
                requests.push_back(req);

            // hit_stats.serve(sectors);
            // hit_stats.fetch(block_sectors);

            _offset += (limit2 - start_sector) * 512;
            start_sector = limit2;
            if (limit2 == backend_it->limit())
                backend_it++;
        }
    }
}
