#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "backend.h"
#include "image.h"
#include "journal.h"
#include "lsvd_types.h"
#include "shared_read_cache.h"

extern int init_wcache(int fd, uuid_t &uuid, int n_pages);
const int block_sectors = CACHE_CHUNK_SIZE / 512;

lsvd_image::~lsvd_image()
{
    // TODO fix to the utterly cursed try_open function so that the object is
    // always in valid state instaed of being partially constructed
    if (wcache) {
        wcache->flush();
        wcache->do_write_checkpoint();
    }
    if (xlate && !cfg.no_gc)
        xlate->stop_gc();
    if (xlate)
        xlate->checkpoint();
    if (write_fd >= 0)
        close(write_fd);
}

uptr<lsvd_image> lsvd_image::open_image(std::string name, rados_ioctx_t io)
{
    uptr<lsvd_image> img;
    try {
        img->try_open(name, io);
        return img;
    } catch (std::exception &e) {
        log_error("Failed to open image {}: {}", name, e.what());
        return nullptr;
    }
}

int lsvd_image::try_open(std::string name, rados_ioctx_t io)
{
    this->image_name = name;

    if (cfg.read() < 0)
        throw std::runtime_error("Failed to read config");

    objstore = make_rados_backend(io);
    shared_cache =
        get_read_cache_instance(cfg.rcache_dir, cfg.cache_size, objstore);

    /* read superblock and initialize translation layer
     */
    xlate = make_translate(objstore, &cfg, &map, &bufmap, &map_lock,
                           &bufmap_lock, shared_cache);
    size = xlate->init(name.c_str(), true);
    check_cond(size < 0, "Failed to initialize translation layer err={}", size);

    /* figure out cache file name, create it if necessary
     */

    /*
     * TODO: Open 2 files. One for wcache and one for reader
     */
    std::string wcache_name =
        cfg.cache_filename(xlate->uuid, name.c_str(), LSVD_CFG_WRITE);

    if (access(wcache_name.c_str(), R_OK | W_OK) < 0) {
        log_info("Creating write cache file {}", wcache_name);
        int cache_pages = cfg.wlog_size / 4096;

        int fd = open(wcache_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
        check_ret_errno(fd, "Can't open wcache file");

        if (init_wcache(fd, xlate->uuid, cache_pages) < 0)
            throw std::runtime_error("Failed to initialize write cache");
        close(fd);
    }

    write_fd = open(wcache_name.c_str(), O_RDWR);
    check_ret_errno(write_fd, "Can't open wcache file");

    j_write_super *jws = (j_write_super *)aligned_alloc(512, 4096);

    check_ret_errno(pread(write_fd, (char *)jws, 4096, 0),
                    "Can't read wcache superblock");
    if (jws->magic != LSVD_MAGIC || jws->type != LSVD_J_W_SUPER)
        throw std::runtime_error("bad magic/type in write cache superblock\n");
    if (memcmp(jws->vol_uuid, xlate->uuid, sizeof(uuid_t)) != 0)
        throw std::runtime_error("object and cache UUIDs don't match");

    wcache = make_write_cache(0, write_fd, xlate.get(), &cfg);
    free(jws);

    if (!cfg.no_gc)
        xlate->start_gc();
    return 0;
}

/**
 * This is the base for aio read and write requests. It's copied from
 * the old rbd_aio_req omniclass, with the read and write paths split out and
 * the common completion handling moved here.
 */
class lsvd_image::aio_request : public self_refcount_request
{
  private:
    std::function<void(int)> cb;
    std::atomic_flag done = false;

  protected:
    lsvd_image *img = nullptr;
    smartiov iovs;

    size_t req_offset;
    size_t req_bytes;

    aio_request(lsvd_image *img, size_t offset, smartiov iovs,
                std::function<void(int)> cb)
        : cb(cb), img(img), iovs(iovs), req_offset(offset)
    {
        req_bytes = iovs.bytes();
    }

    void complete_request(int val)
    {
        cb(val);
        done.test_and_set(std::memory_order_seq_cst);
        done.notify_all();
        dec_and_free();
    }

  public:
    inline virtual void wait() override
    {
        refcount++;
        done.wait(false, std::memory_order_seq_cst);
        dec_and_free();
    }
};

class lsvd_image::read_request : public lsvd_image::aio_request
{
  private:
    std::atomic_int num_subreqs = 0;

  public:
    read_request(lsvd_image *img, size_t offset, smartiov iovs,
                 std::function<void(int)> cb)
        : aio_request(img, offset, iovs, cb)
    {
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);

        std::vector<request *> requests;
        img->handle_reads(req_offset, iovs, requests);
        num_subreqs = requests.size();

        // We might sometimes instantly complete; in that case, there will be
        // no notifiers, we must notify ourselves.
        // NOTE lookout for self-deadlock
        if (num_subreqs == 0) {
            notify(nullptr);
        } else {
            for (auto const &r : requests)
                r->run(this);
        }
    }

    void notify(request *child) override
    {
        free_child(child);

        auto old = num_subreqs.fetch_sub(1, std::memory_order_seq_cst);
        if (old > 1)
            return;

        complete_request(req_bytes);
    }
};

void lsvd_image::handle_reads(size_t offset, smartiov iovs,
                              std::vector<request *> &requests)
{
    sector_t start_sector = offset / 512;
    sector_t end_sector = start_sector + iovs.bytes() / 512;

    /**
     * These locks are too coarse, they block everything
     */
    std::unique_lock lk(reader_lock);
    std::shared_lock lk2(map_lock);
    std::unique_lock lk3(bufmap_lock);

    auto bufmap_it = bufmap.end();
    if (bufmap.size() > 0)
        bufmap_it = bufmap.lookup(start_sector);
    auto backend_it = map.lookup(start_sector);
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
        if (bufmap_it != bufmap.end())
            std::tie(base1, limit1, bufptr) =
                bufmap_it->vals(start_sector, end_sector);

        sector_t base2 = end_sector, limit2 = end_sector;
        extmap::obj_offset objptr = {0, 0};
        if (backend_it != map.end())
            std::tie(base2, limit2, objptr) =
                backend_it->vals(start_sector, end_sector);

        /* [base..base3] is unmapped - zero it out
         */
        auto base3 = std::min(base1, base2);
        if (base3 > start_sector) {
            sector_t sectors = base3 - start_sector;
            iovs.zero(_offset, _offset + sectors * 512L);
            start_sector = base3;
            _offset += sectors * 512L;
            continue;
        }

        /* buffer map (i.e. current batch buffer) takes priority
         */
        if (base1 == start_sector) {
            sector_t sectors = limit1 - start_sector;
            auto slice = iovs.slice(_offset, _offset + sectors * 512L);
            slice.copy_in(bufptr.buf);
            // hit_stats.serve(sectors);

            start_sector = limit1;
            _offset += sectors * 512;

            /* skip any objmap extents fully obscured by this extent:
             *  it1:  |---------------------|
             *  it2:    |----|    |----|
             *                           |------|   < but not this
             */
            while (backend_it != map.end() && backend_it->limit() <= limit1)
                backend_it++;
            bufmap_it++;
            continue;
        }

        assert(base2 == start_sector);
        assert(backend_it != map.end());
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
            auto slice = iovs.slice(_offset, _offset + sectors * 512L);

            objname on(xlate->prefix(objptr.obj), objptr.obj);
            // TODO old code called translate::object_read_start/end here
            // to pin the objects, figure out how to replicate
            auto req = objstore->aio_read(on.str(), objptr.offset * 512, slice);
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
            auto slice = iovs.slice(_offset, _offset + sectors * 512L);
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
            auto prefix = xlate->prefix(key.obj);
            auto req =
                shared_cache->make_read_req(prefix, key.obj, key.offset * 512L,
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

class lsvd_image::write_request : public lsvd_image::aio_request
{
  private:
    std::atomic_int n_req = 0;
    /**
     * Not quite sure we own these iovs; we should transfer ownership to writev
     * and be done with it. The old code had these as pointers, but changed
     * them to be in the vectwor.
     */
    std::vector<smartiov> sub_iovs;

  public:
    write_request(lsvd_image *img, size_t offset, smartiov iovs,
                  std::function<void(int)> cb)
        : aio_request(img, offset, iovs, cb)
    {
    }

    void notify(request *req) override
    {
        free_child(req);
        auto old = n_req.fetch_sub(1, std::memory_order_seq_cst);
        if (old > 1)
            return;

        img->wcache->release_room(req_bytes / 512);
        complete_request(0); // TODO shouldn't we return bytes written?
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);

        img->wcache->get_room(req_bytes / 512);
        img->xlate->wait_for_room();

        sector_t size_sectors = req_bytes / 512;

        // split large requests into 2MB (default) chunks
        sector_t max_sectors = img->cfg.wcache_chunk / 512;
        n_req += div_round_up(req_bytes / 512, max_sectors);
        // TODO: this is horribly ugly

        std::vector<request *> requests;
        auto cur_offset = req_offset;

        for (sector_t s_offset = 0; s_offset < size_sectors;
             s_offset += max_sectors) {
            auto _sectors = std::min(size_sectors - s_offset, max_sectors);
            smartiov tmp =
                iovs.slice(s_offset * 512L, s_offset * 512L + _sectors * 512L);
            smartiov _iov(tmp.data(), tmp.size());
            sub_iovs.push_back(_iov);
            auto req = img->wcache->writev(cur_offset / 512, &_iov);
            requests.push_back(req);

            cur_offset += _sectors * 512L;
        }

        for (auto r : requests)
            r->run(this);
    }
};

class trim_request : public lsvd_image::aio_request
{
  public:
    trim_request(lsvd_image *img, size_t offset, size_t len,
                 std::function<void(int)> cb)
        : aio_request(img, offset, smartiov(), cb)
    {
        req_bytes = len;
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);
        img->xlate->trim(req_offset, req_bytes);
        complete_request(0);
    }

    void notify(request *req) override { UNIMPLEMENTED(); }
};

class flush_request : public lsvd_image::aio_request
{
  public:
    flush_request(lsvd_image *img, std::function<void(int)> cb)
        : aio_request(img, 0, smartiov(), cb)
    {
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);
        img->xlate->flush();
        complete_request(0);
    }

    void notify(request *req) override { UNIMPLEMENTED(); }
};

request *lsvd_image::read(size_t offset, smartiov iov,
                          std::function<void(int)> cb)
{
    return new read_request(this, offset, iov, cb);
}

request *lsvd_image::write(size_t offset, smartiov iov,
                           std::function<void(int)> cb)
{
    return new write_request(this, offset, iov, cb);
}

request *lsvd_image::trim(size_t offset, size_t len,
                          std::function<void(int)> cb)
{
    return new trim_request(this, offset, len, cb);
}

request *lsvd_image::flush(std::function<void(int)> cb)
{
    return new flush_request(this, cb);
}
