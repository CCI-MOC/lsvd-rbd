#include <fcntl.h>
#include <rados/librados.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>

#include "backend.h"
#include "image.h"
#include "lsvd_types.h"
#include "objects.h"
#include "shared_read_cache.h"
#include "utils.h"
#include "write_cache.h"

const int block_sectors = CACHE_CHUNK_SIZE / 512;

LsvdImage::LsvdImage(str name, rados_ioctx_t io, lsvd_config cfg_)
    : imgname(name), cfg(cfg_), io(io)
{
    objstore = make_rados_backend(io);
    rcache =
        get_read_cache_instance(cfg.rcache_dir, cfg.rcache_bytes, objstore);

    read_superblock().value();
    debug("Found checkpoints: {}", checkpoints);
    if (checkpoints.size() > 0)
        read_from_checkpoint(checkpoints.back()).value();

    // Roll forward on the log
    auto last_data_seq = roll_forward_from_last_checkpoint();
    debug("Last data seq: {}", last_data_seq);

    // Successfully recovered everything, now we have enough information to
    // init everything else
    xlate = make_translate(name, cfg, size, uuid, objstore, rcache, objmap,
                           map_lock, bufmap, bufmap_lock, last_data_seq, clones,
                           obj_info, checkpoints);

    wlog = open_wlog(cfg.wlog_path(name), *xlate, cfg);
    THROW_MSG_ON(!wlog, "Failed to open write log");
    // recover_from_wlog();

    log_info("Image '{}' opened successfully", name);
}

LsvdImage::~LsvdImage()
{
    wlog->flush();
    wlog->do_write_checkpoint();
    xlate->shutdown();

    // TODO figure out who owns the rados connection
    rados_ioctx_destroy(io);

    log_info("Image '{}' closed", imgname);
}

Result<void> LsvdImage::apply_log(seqnum_t seq)
{
    object_reader parser(objstore);
    auto data_hdr =
        BOOST_OUTCOME_TRYX(parser.read_data_hdr(oname(imgname, seq)));
    trace("Recovering log with object at seq {}", seq);

    auto ohdr = data_hdr.hdr;
    if (ohdr->type == OBJ_CHECKPOINT) {
        log_warn("CORRUPTION: Found checkpoint at seq {} that was not "
                 "present in the superblock.",
                 seq);
        checkpoints.push_back(seq);
        return outcome::success();
    }

    obj_info[seq] = (data_obj_info){
        .hdr = ohdr->hdr_sectors,
        .data = ohdr->data_sectors,
        .live = ohdr->data_sectors,
    };

    // Consume log records
    sector_t offset = 0;
    vec<extmap::lba2obj> deleted;
    for (auto dmap : data_hdr.data_map) {
        // Update the extent map
        extmap::obj_offset oo = {seq, offset + ohdr->hdr_sectors};
        objmap.update(dmap->lba, dmap->lba + dmap->len, oo, &deleted);
        offset += dmap->len;
    }

    // Manage deleted extents
    for (auto d : deleted) {
        auto [base, limit, ptr] = d.vals();
        obj_info[ptr.obj].live -= (limit - base);
        THROW_MSG_ON(obj_info[ptr.obj].live >= 0, "Negative live sectors.");
    }

    return outcome::success();
}

Result<void> LsvdImage::read_superblock()
{
    object_reader parser(objstore);
    auto superblock = BOOST_OUTCOME_TRYX(parser.read_superblock(imgname));

    size = superblock.vol_size;
    uuid_copy(uuid, superblock.uuid);

    for (auto ckpt : superblock.ckpts)
        checkpoints.push_back(ckpt);

    for (auto ci : superblock.clones) {
        clone_base c;
        c.name = std::string(ci->name, ci->name_len);
        c.last_seq = ci->last_seq;
        c.first_seq = ci->last_seq + 1;

        debug("Using base image {} upto seq {}", c.name, c.last_seq);
        clones.push_back(c);
    }

    return outcome::success();
}

Result<void> LsvdImage::read_from_checkpoint(seqnum_t seq)
{
    object_reader parser(objstore);
    auto parsed =
        BOOST_OUTCOME_TRYX(parser.read_checkpoint(oname(imgname, seq)));

    for (auto obj : parsed.objects) {
        obj_info[obj->seq] = (data_obj_info){
            .hdr = obj->hdr_sectors,
            .data = obj->data_sectors,
            .live = obj->live_sectors,
        };
    }

    for (auto m : parsed.dmap) {
        extmap::obj_offset oo = {m->obj, m->offset};
        objmap.update(m->lba, m->lba + m->len, oo);
    }

    return outcome::success();
}

// Returns last processed object's seqnum
seqnum_t LsvdImage::roll_forward_from_last_checkpoint()
{
    if (checkpoints.size() == 0)
        return 0;

    object_reader parser(objstore);
    auto last_ckpt = checkpoints.back();
    auto seq = last_ckpt + 1;

    for (;; seq++) {
        auto ret = apply_log(seq);
        if (!ret)
            break;
    }

    seq -= 1;

    // Delete "dangling" objects if there are any in case they cause trouble
    // with corruption
    // This must be larger than the max backend batch size to avoid
    // potential corruption if subsequent breaks overlap with current dangling
    // objects and we get writes from two different "generations"
    for (seqnum_t i = 1; i < cfg.backend_write_window * 4; i++)
        std::ignore = objstore->delete_obj(oname(imgname, seq + i));

    return seq;
}

void LsvdImage::recover_from_wlog() { UNIMPLEMENTED(); }

/**
 * This is the base for aio read and write requests. It's copied from
 * the old rbd_aio_req omniclass, with the read and write paths split out and
 * the common completion handling moved here.
 */
class LsvdImage::aio_request : public self_refcount_request
{
  private:
    std::function<void(int)> cb;
    std::atomic_flag done = false;

  protected:
    LsvdImage *img = nullptr;
    smartiov iovs;

    size_t req_offset;
    size_t req_bytes;

    aio_request(LsvdImage *img, size_t offset, smartiov iovs,
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

class LsvdImage::read_request : public LsvdImage::aio_request
{
  private:
    std::atomic_int num_subreqs = 0;

  public:
    read_request(LsvdImage *img, size_t offset, smartiov iovs,
                 std::function<void(int)> cb)
        : aio_request(img, offset, iovs, cb)
    {
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);

        vec<request *> requests;
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

void LsvdImage::handle_reads(size_t offset, smartiov iovs,
                              vec<request *> &requests)
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
    auto backend_it = objmap.lookup(start_sector);
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
        if (backend_it != objmap.end())
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
            while (backend_it != objmap.end() && backend_it->limit() <= limit1)
                backend_it++;
            bufmap_it++;
            continue;
        }

        assert(base2 == start_sector);
        assert(backend_it != objmap.end());
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
            auto req = rcache->make_read_req(prefix, key.obj, key.offset * 512L,
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

class LsvdImage::write_request : public LsvdImage::aio_request
{
  private:
    std::atomic_int n_req = 0;
    /**
     * Not quite sure we own these iovs; we should transfer ownership to writev
     * and be done with it. The old code had these as pointers, but changed
     * them to be in the vectwor.
     */
    vec<smartiov> sub_iovs;

  public:
    write_request(LsvdImage *img, size_t offset, smartiov iovs,
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

        img->wlog->release_room(req_bytes / 512);
        complete_request(0); // TODO shouldn't we return bytes written?
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);

        img->wlog->reserve_room(req_bytes / 512);
        img->xlate->backend_backpressure();

        sector_t size_sectors = req_bytes / 512;

        // split large requests into 2MB (default) chunks
        sector_t max_sectors = img->cfg.wlog_chunk_bytes / 512;
        n_req += div_round_up(req_bytes / 512, max_sectors);
        // TODO: this is horribly ugly

        vec<request *> requests;
        auto cur_offset = req_offset;

        for (sector_t s_offset = 0; s_offset < size_sectors;
             s_offset += max_sectors) {
            auto _sectors = std::min(size_sectors - s_offset, max_sectors);
            smartiov tmp =
                iovs.slice(s_offset * 512L, s_offset * 512L + _sectors * 512L);
            smartiov _iov(tmp.data(), tmp.size());
            sub_iovs.push_back(_iov);
            auto req = img->wlog->writev(cur_offset / 512, &_iov);
            requests.push_back(req);

            cur_offset += _sectors * 512L;
        }

        for (auto r : requests)
            r->run(this);
    }
};

class trim_request : public LsvdImage::aio_request
{
  public:
    trim_request(LsvdImage *img, size_t offset, size_t len,
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

class flush_request : public LsvdImage::aio_request
{
  public:
    flush_request(LsvdImage *img, std::function<void(int)> cb)
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

request *LsvdImage::read(size_t offset, smartiov iov,
                          std::function<void(int)> cb)
{
    return new read_request(this, offset, iov, cb);
}

request *LsvdImage::write(size_t offset, smartiov iov,
                           std::function<void(int)> cb)
{
    return new write_request(this, offset, iov, cb);
}

request *LsvdImage::trim(size_t offset, size_t len,
                          std::function<void(int)> cb)
{
    return new trim_request(this, offset, len, cb);
}

request *LsvdImage::flush(std::function<void(int)> cb)
{
    return new flush_request(this, cb);
}

Result<void> LsvdImage::create_new(str name, usize size, rados_ioctx_t io)
{
    auto be = make_rados_backend(io);
    auto parser = object_reader(be);

    uuid_t uuid;
    uuid_generate_random(uuid);

    vec<byte> buf(4096);
    vec<seqnum_t> ckpts;
    vec<clone_base> clones;
    serialise_superblock(buf, ckpts, clones, uuid, size);

    auto rc = BOOST_OUTCOME_TRYX(be->write(name, buf.data(), buf.size()));
    PR_RET_IF(std::cmp_not_equal(rc, buf.size()), LsvdError::MissingData,
              "Failed to write superblock '{}'", name);
    return outcome::success();
}

Result<void> LsvdImage::get_uuid(str name, uuid_t &uuid, rados_ioctx_t io)
{
    auto be = make_rados_backend(io);
    auto parser = object_reader(be);
    auto osb = BOOST_OUTCOME_TRYX(parser.read_superblock(name));
    uuid_copy(uuid, osb.uuid);
    return outcome::success();
}

Result<void> LsvdImage::delete_image(str name, rados_ioctx_t io)
{
    auto be = make_rados_backend(io);
    auto parser = object_reader(be);
    auto sb = BOOST_OUTCOME_TRYX(parser.read_superblock(name));

    seqnum_t seq;
    for (auto ckpt : sb.ckpts) {
        BOOST_OUTCOME_TRYX(be->delete_obj(oname(name, ckpt)));
        seq = ckpt;
    }

    for (int n = 0; n < 16; seq++, n++)
        if (be->delete_obj(oname(name, seq)).has_value())
            n = 0;

    // delete the superblock last so we can recover from partial deletion
    return be->delete_obj(name);
}

Result<void> LsvdImage::clone_image(str oldname, str newname, rados_ioctx_t io)
{
    UNIMPLEMENTED();
}