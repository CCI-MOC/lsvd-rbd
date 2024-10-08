#include <boost/outcome/try.hpp>
#include <folly/experimental/coro/Invoke.h>
#include <folly/logging/xlog.h>
#include <zpp_bits.h>

#include "config.h"
#include "folly/String.h"
#include "image.h"
#include "read_cache.h"
#include "representation.h"
#include "smartiov.h"
#include "utils.h"

class LogObj
{
  public:
    const seqnum_t seqnum;

  private:
    u32 bytes_written = 0;
    vec<byte> data;

    // We start with one to mark that we're expecting further writes, only when
    // we get mark as completed to we let it go to 0
    std::atomic<u32> writes_pending = 1;
    folly::coro::Baton writes_done_baton;
    folly::coro::Baton flush_done_baton;

  public:
    LogObj(seqnum_t seqnum, usize size) : seqnum(seqnum), data(size) {}
    ~LogObj() {}

    auto append(usize len) -> std::pair<S3Ext, byte *>
    {
        XLOGF(DBG9, "Appending seq {:#x} len={}, written={}", seqnum, len,
              bytes_written);
        ENSURE(bytes_written + len <= data.size());
        auto ret = bytes_written;
        bytes_written += len;
        return {S3Ext{seqnum, ret, len}, data.data() + ret};
    }

    auto remaining() { return data.size() - bytes_written; }
    auto as_iov() { return iovec{data.data(), bytes_written}; }
    auto as_buffer() { return buffer{data.data(), bytes_written}; }
    auto at(S3Ext ext) { return data.data() + ext.offset; }

    Task<void> wait_for_writes() { co_await writes_done_baton; }
    Task<void> wait_for_flush() { co_await flush_done_baton; }
    void flush_done() { flush_done_baton.post(); }
    void mark_complete() { write_end(); }
    void write_start() { writes_pending.fetch_add(1); }
    void write_end()
    {
        if (writes_pending.fetch_sub(1) == 1)
            writes_done_baton.post();
    }
};

ResTask<void> LsvdImage::read(off_t offset, smartiov iovs)
{
    auto data_bytes = iovs.bytes();
    XLOGF(DBG8, " SR off={} len={}", offset, data_bytes);

    ENSURE(offset >= 0);
    ENSURE(iovs.bytes() > 0 && iovs.bytes() % sector_size == 0);
    if (offset + iovs.bytes() > superblock.image_size)
        co_return outcome::failure(std::errc::invalid_argument);

    // Since backend objects are immutable, we just need a read lock for as long
    // as we're getting the extents. Once we have the extents, the map can be
    // moified and we don't care.
    auto exts = co_await extmap->lookup(offset, iovs.bytes());

    // MAYBE: optimise for the case where len(exts) == 1

    fvec<folly::SemiFuture<Result<void>>> tasks;
    tasks.reserve(exts.size());

    // We can hold on to this for a while since we only write to the map on
    // rollover, and those are fairly rare compared to reads
    for (auto &[ext_img_off, ext] : exts) {
        auto base = ext_img_off - offset;

        // Unmapped range; zero it and move on
        if (ext.seqnum == 0) {
            iovs.zero(base, ext.len);
            continue;
        }

        auto ext_iov = iovs.slice(base, ext.len);

        // First try to read from the write log in case it's in memory
        auto l = co_await pending_mtx.co_scoped_lock_shared();
        auto it = pending_objs.find(ext.seqnum);
        if (it != pending_objs.end()) {
            // shared_ptr to logobj; will pin it in memory until scope ends
            sptr<LogObj> obj = it->second;
            l.unlock();

            auto ptr = obj->at(ext);
            iovs.copy_in(ptr, ext.len);
            continue;
        }
        l.unlock();

        if (ENABLE_SEQUENTIAL_DEBUG_READS) {
            auto res = co_await cache->read(ext, ext_iov);
            DEBUG_IF_FAIL(res);
        } else {
            tasks.push_back(cache->read(ext, ext_iov).semi());
        }
        // Not in pending logobjs, get it from the read-through backend cache
        tasks.push_back(cache->read(ext, ext_iov).semi());
    }

    auto all = co_await folly::collectAll(tasks);
    for (auto &t : all)
        if (t.hasException())
            co_return outcome::failure(std::errc::io_error);
        else if (t->has_error())
            co_return t->as_failure();

    XLOGF(DBG9, "ER off={} len={}", offset, data_bytes);
    co_return outcome::success();
}

/**
The main concern is maintaining consistency between the write log and the
backend object; we need to ensure that writes in both places happen identical
orders to prevent data changes on recovery.

The method with which we achieve this is:

- Reserve space in backend the first thing, getting (seqnum, off, len)
    - Increment a refcount for pending writes to that object
    - We can't write it to the backend until this is 0 is and no longer current
    - The object is also pinned in memory until we flush
    - We DO NOT update the extent map at this point, so reads are not effected
- Write the operation to the write log, including the reserved triplet
    - Because we con't flush the object to the backend until this is done, we
      will never have writes in the backend that aren't in the write log
    - This ensures ordering on crash and replay as well
- Update the extent map, meaning we can now serve reads with the write
    - This is the only time we need to grab a global write lock
    - Decrement the refcount after, since we no longer need to pin
- Ack the write

In essence, we are treating (seqnum, off) as an atomically increasing write
counter for ordering purposes. Even if they're written out of order to the nvme,
we just need to replay them in-order on recovery.

We also need to consider how this interacts with write barriers (flush). Flush
requires that all writes that *complete* before its submission are written out.

One thing to realise is that we can also treat the seqnum as an epoch. So on
flush we immediately declare a new epoch and wait for all writes of the previous
epoch to complete. Since we already have a refcount, as soon as that previous
epoch's refcount is 0, we can:

(1) flush it to the backend (thus unpinning it from memory), and
(2) ack the flush

We don't have to interrupt other pending writes to do this even, we just need to
ensure that the flush is the last thing that happens in the previous epoch. We
avoid locking anything for too long by doing this and ensure maximum
parallelism.

*/
ResTask<void> LsvdImage::write(off_t offset, smartiov iovs)
{
    auto data_bytes = iovs.bytes();
    XLOGF(DBG8, "SW off={} len={}", offset, data_bytes);

    ENSURE(offset >= 0);
    ENSURE(data_bytes > 0 && data_bytes % sector_size == 0);

    // pin the current object to reserve space in the logobj, and while the
    // lock is held check if we have to rollover
    auto lck = co_await logobj_mtx.co_scoped_lock();
    auto obj = cur_logobj;
    obj->write_start();

    // INVARIANT: the logobj always has enough space for 1 more write
    // (max_rw_size in bdev_lsvd.cc). Only after this write is done do we
    // check if we need to rollover to maintain this invariant
    auto [ext, buf] = obj->append(data_bytes + LOG_ENTRY_SIZE);
    co_await rollover_log(false);
    lck.unlock();

    // Write out the data
    auto *entry = reinterpret_cast<log_entry *>(buf);
    *entry = log_entry{
        .type = log_entry_type::WRITE,
        .offset = static_cast<u64>(offset),
        .len = data_bytes,
    };
    iovs.copy_out(entry->data);

    // Write it to the journal
    BOOST_OUTCOME_CO_TRY(
        co_await journal->record_write(offset, iovec{buf, data_bytes}, ext));

    // Update the extent map and ack. The extent map takes extents that do not
    // include headers, so strip those off before updating.
    auto data_ext = get_data_ext(ext);
    co_await extmap->update(offset, data_bytes, data_ext);

    obj->write_end();

    XLOGF(DBG9, "EW off={} len={}", offset, data_bytes);
    co_return outcome::success();
}

ResTask<void> LsvdImage::trim(off_t offset, usize len)
{
    XLOGF(DBG8, "ST off={} len={}", offset, len);
    ENSURE(offset >= 0);
    if (len == 0)
        co_return outcome::success();

    auto ol = co_await logobj_mtx.co_scoped_lock();
    auto obj = cur_logobj;
    obj->write_start();
    auto [ext, buf] = obj->append(LOG_ENTRY_SIZE);
    co_await rollover_log(false);
    ol.unlock();

    // Write out the data
    auto *entry = reinterpret_cast<log_entry *>(buf);
    *entry = log_entry{
        .type = log_entry_type::TRIM,
        .offset = static_cast<u64>(offset),
        .len = len,
    };

    BOOST_OUTCOME_CO_TRY(co_await journal->record_trim(offset, len, ext));
    co_await extmap->unmap(offset, len);

    obj->write_end();
    XLOGF(DBG9, "ET off={} len={}", offset, len);
    co_return outcome::success();
}

ResTask<void> LsvdImage::flush()
{
    XLOGF(DBG8, "SFlush");
    auto ol = co_await logobj_mtx.co_scoped_lock();
    auto obj = co_await rollover_log(true);
    ol.unlock();

    co_await obj->wait_for_writes();
    XLOGF(DBG9, "EFlush");
    co_return outcome::success();
}

// Assumes that the logobj_mtx is held exclusively
Task<sptr<LogObj>> LsvdImage::rollover_log(bool force)
{
    if (!force && cur_logobj->remaining() > rollover_threshold) {
        co_return cur_logobj;
    }

    auto exe = co_await folly::coro::co_current_executor;
    auto prev = cur_logobj;
    auto new_seqnum = prev->seqnum + 1;

    XLOGF(DBG7, "Rollover log from {:#x} to {:#x}", prev->seqnum, new_seqnum);

    // add new checkpoint if needed
    if (new_seqnum > last_checkpoint + checkpoint_interval_epoch) {
        auto ckpt_buf = co_await extmap->serialise();
        checkpoint(new_seqnum, std::move(ckpt_buf)).scheduleOn(exe).start();
        last_checkpoint = new_seqnum;
        new_seqnum += 1;
    }

    // this is where we allocate new log objects
    // POTENTIAL OPTIMISATION: we could start recycling objects, or just
    // manually manage the allocations (ex alloc rdma memory)
    auto new_logobj = std::make_shared<LogObj>(new_seqnum, max_log_size);
    {
        auto l = co_await pending_mtx.co_scoped_lock();
        auto [it, inserted] = pending_objs.emplace(new_seqnum, new_logobj);
        ENSURE(inserted == true);
    }

    cur_logobj = new_logobj;
    prev->mark_complete();

    flush_logobj(prev).scheduleOn(exe).start();
    co_return prev;
}

ResTask<void> LsvdImage::flush_logobj(sptr<LogObj> obj)
{
    XLOGF(DBG8, "Flushing log object {:#x}", obj->seqnum);
    co_await obj->wait_for_writes();
    auto k = get_logobj_key(name, obj->seqnum);

    auto obj_iov = obj->as_iov();
    obj_sizes.wlock()->emplace(obj->seqnum, obj_iov.iov_len);

    // TODO think about what to do in the case of failure here
    BOOST_OUTCOME_CO_TRYX(co_await s3->write(k, obj->as_iov()));
    XLOGF(DBG, "Flushed log object {:#x}", obj->seqnum);

    BOOST_OUTCOME_CO_TRYX(
        co_await cache->insert_obj(obj->seqnum, obj->as_buffer()));

    auto l = co_await pending_mtx.co_scoped_lock();
    pending_objs.erase(obj->seqnum);

    obj->flush_done();
    co_return outcome::success();
}

ResTask<void> LsvdImage::checkpoint(seqnum_t seqnum, vec<byte> buf)
{
    auto k = get_logobj_key(name, seqnum);

    // TODO handle failure
    BOOST_OUTCOME_CO_TRYX(co_await s3->write(k, iovec{buf.data(), buf.size()}));

    // update superblock with new checkpoint
    superblock.checkpoints.push_back(seqnum);
    auto super_buf = superblock.serialise().value();

    BOOST_OUTCOME_CO_TRYX(
        co_await s3->write(name, iovec{super_buf.data(), super_buf.size()}));

    co_return outcome::success();
}

ResTask<void> LsvdImage::replay_obj(seqnum_t seq, vec<byte> buf,
                                    usize start_byte)
{
    XLOGF(DBG8, "Replaying log object {:#x}", seq);

    u32 consumed_bytes = start_byte;
    while (consumed_bytes < buf.size()) {
        auto entry = reinterpret_cast<log_entry *>(buf.data() + consumed_bytes);
        switch (entry->type) {
        case log_entry_type::WRITE:
            co_await extmap->update(entry->offset, entry->len,
                                    S3Ext{seq, consumed_bytes, entry->len});
            consumed_bytes += sizeof(log_entry) + entry->len;
            break;
        case log_entry_type::TRIM:
            co_await extmap->unmap(entry->offset, entry->len);
            consumed_bytes += sizeof(log_entry);
            break;
        default:
            XLOGF(ERR, "Unknown log entry type {}",
                  static_cast<u32>(entry->type));
            co_return outcome::failure(std::errc::io_error);
        }
    }

    co_return outcome::success();
}

ResTask<uptr<LsvdImage>> LsvdImage::mount(sptr<ObjStore> s3, fstr name,
                                          fstr cfg_str)
{
    XLOGF(INFO, "Mounting {} with config {}", name, cfg_str);
    auto img = std::unique_ptr<LsvdImage>(new LsvdImage(name));

    img->cfg = BOOST_OUTCOME_CO_TRYX(LsvdConfig::parse(name, cfg_str));

    // get superblock and parse it
    SuperblockInfo superblock;
    auto super_buf = BOOST_OUTCOME_CO_TRYX(co_await s3->read_all(name));
    BOOST_OUTCOME_CO_TRYX(superblock.deserialise(super_buf));
    img->superblock = superblock;
    XLOGF(INFO, "Found image of size {}", superblock.image_size);
    XLOGF(INFO, "Found checkpoints: [{}]",
          folly::join(", ", superblock.checkpoints));
    XLOGF(INFO, "Found snapshots: [{}]",
          folly::join(", ", superblock.snapshots));

    // deserialise the last checkpoint
    img->last_checkpoint = superblock.checkpoints.back();
    auto last_ckpt_key = get_logobj_key(name, img->last_checkpoint);
    auto last_ckpt_buf =
        BOOST_OUTCOME_CO_TRYX(co_await s3->read_all(last_ckpt_key));
    img->extmap = ExtMap::deserialise(last_ckpt_buf);
    XLOGF(INFO, "Deserialised checkpoint {:#x}", img->last_checkpoint.load());
    // XLOGF(DBG9, "Extmap {}", co_await img->extmap->to_string());

    // build the cache and journal
    img->s3 = s3;
    img->cache = ReadCache::make_image_cache(s3, name);
    img->journal =
        BOOST_OUTCOME_CO_TRYX(co_await Journal::open(img->cfg.journal_path));

    // recover the image starting from the last checkpoint
    u32 err_count = 0;
    seqnum_t last_known_seq = img->last_checkpoint + 1;
    for (auto cur_seq = last_known_seq; err_count < LOG_REPLAY_OBJECT_COUNT;
         cur_seq++) {
        auto lo_buf = co_await s3->read_all(get_logobj_key(name, cur_seq));
        if (!lo_buf.has_value()) {
            err_count++;
            continue;
        }

        BOOST_OUTCOME_CO_TRYX(
            co_await img->replay_obj(cur_seq, lo_buf.value(), 0));
        last_known_seq = cur_seq;
    }

    img->extmap->verify_self_integrity();
    XLOGF(DBG9, "Post-recovery extmap\n{}", co_await img->extmap->to_string());

    // TODO search for uncommited objects in the journal and replay them too

    // create the first log object
    auto seq = last_known_seq + 1;
    auto new_logobj = std::make_shared<LogObj>(seq, img->max_log_size);
    img->pending_objs.emplace(seq, new_logobj);
    img->cur_logobj = new_logobj;

    XLOGF(INFO, "Mounted image {}", name);
    co_return img;
}

Task<void> LsvdImage::unmount()
{
    XLOGF(INFO, "Unmounting image {}", name);
    auto ol = co_await logobj_mtx.co_scoped_lock();
    auto obj = co_await rollover_log(true);
    co_await obj->wait_for_writes();
    co_await obj->wait_for_flush();
}

ResTask<void> LsvdImage::write_and_verify(off_t offset, smartiov iovs)
{
    auto res = co_await write(offset, iovs);
    DEBUG_IF_FAIL(res);
    co_await verify_integrity();
    co_return outcome::success();
}

Task<void> LsvdImage::verify_integrity()
{
    obj_sizes.withRLock(
        [this](auto &locked) { extmap->verify_ext_integrity(locked); });
    co_return;
}

ResTask<void> LsvdImage::create(sptr<ObjStore> s3, fstr name, usize size)
{
    XLOGF(INFO, "Creating new image {}", name);

    XLOGF(DBG, "Writing out checkpoint of empty extmap");
    auto extmap = ExtMap::create_empty(size);
    auto extmap_buf = co_await extmap->serialise();
    auto ckpt_key = get_logobj_key(name, 1);
    BOOST_OUTCOME_CO_TRYX(co_await s3->write(
        ckpt_key, iovec{extmap_buf.data(), extmap_buf.size()}));

    XLOGF(DBG, "Creating superblock for image of size {}", size);
    auto superblock = SuperblockInfo{
        .image_size = size, .clones = {}, .checkpoints = {1}, .snapshots = {}};
    auto super_buf = BOOST_OUTCOME_CO_TRYX(superblock.serialise());
    BOOST_OUTCOME_CO_TRYX(
        co_await s3->write(name, iovec{super_buf.data(), super_buf.size()}));

    co_return outcome::success();
}

ResTask<void> LsvdImage::remove(sptr<ObjStore> s3, fstr name)
{
    XLOGF(INFO, "Removing image {}", name);

    SuperblockInfo sb;
    auto super_buf = BOOST_OUTCOME_CO_TRYX(co_await s3->read_all(name));
    BOOST_OUTCOME_CO_TRYX(sb.deserialise(super_buf));

    // delete everything up to the checkpoint
    auto upto = sb.checkpoints.back();
    for (u32 i = 0; i < upto; i++) {
        auto key = get_logobj_key(name, i);
        std::ignore = co_await s3->remove(key);
    }

    // keep going until we run out of objects
    for (auto i = 0, ec = 0; ec < 32; i++) {
        auto key = get_logobj_key(name, upto + i);
        auto res = co_await s3->remove(key);
        if (res.has_failure())
            ec++;
    }

    // delete superblock
    auto res = co_await s3->remove(name);
    if (res.has_failure()) {
        XLOGF(ERR, "Failed to remove superblock: {}", res.error().message());
        co_return res;
    }

    co_return outcome::success();
}

ResTask<void> LsvdImage::clone(sptr<ObjStore> s3, fstr src, fstr dst)
{
    XLOGF(INFO, "Cloning image {} to {}", src, dst);
    // TODO
    co_return outcome::failure(std::errc::not_supported);
}
