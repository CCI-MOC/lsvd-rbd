#include <boost/outcome/try.hpp>
#include <folly/experimental/coro/Invoke.h>
#include <folly/logging/xlog.h>
#include <zpp_bits.h>

#include "folly/String.h"
#include "image.h"
#include "representation.h"
#include "smartiov.h"
#include "src/read_cache.h"
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

  public:
    LogObj(seqnum_t seqnum, usize size) : seqnum(seqnum), data(size) {}
    ~LogObj() { assert(writes_pending == 0); }

    auto append(usize len) -> std::pair<S3Ext, byte *>
    {
        assert(bytes_written + len <= data.size());
        auto ret = bytes_written;
        bytes_written += len;
        return {S3Ext{seqnum, ret, len}, data.data() + ret};
    }

    auto remaining() { return data.size() - bytes_written; }
    auto as_iov() { return iovec{data.data(), bytes_written}; }
    auto as_buffer() { return buffer{data.data(), bytes_written}; }
    auto at(S3Ext ext) { return data.data() + ext.offset; }

    Task<void> wait_for_writes() { co_await writes_done_baton; }
    void mark_complete() { write_end(); }
    void write_start() { writes_pending++; }
    void write_end()
    {
        if (writes_pending.fetch_sub(1) == 1)
            writes_done_baton.post();
    }
};

ResTask<void> LsvdImage::read(off_t offset, smartiov iovs)
{
    // Since backend objects are immutable, we just need a read lock for as long
    // as we're getting the extents. Once we have the extents, the map can be
    // moified and we don't care.
    auto exts = co_await extmap->lookup(offset, iovs.bytes());

    folly::fbvector<folly::SemiFuture<Result<void>>> tasks;
    tasks.reserve(exts.size());

    // We can hold on to this for a while since we only write to the map on
    // rollover, and those are fairly rare compared to reads
    auto l = co_await pending_mtx.co_scoped_lock_shared();
    for (auto &[img_off, ext] : exts) {
        auto base = img_off - offset;

        // Unmapped range; zero it and move on
        if (ext.seqnum == 0) {
            iovs.zero(base, base + ext.len);
            continue;
        }

        auto ext_iov = iovs.slice(base, ext.len);

        // First try to read from the write log in case it's in memory
        auto it = pending_objs.find(ext.seqnum);
        if (it != pending_objs.end()) {
            auto ptr = it->second->at(ext);
            iovs.copy_in(ptr, ext.len);
            continue;
        }

        // Not in pending logobjs, get it from the read-through backend cache
        tasks.push_back(cache->read(ext, ext_iov).semi());
    }
    l.unlock();

    auto all = co_await folly::collectAll(tasks);
    for (auto &t : all)
        if (t.hasException())
            co_return outcome::failure(std::errc::io_error);
        else
            BOOST_OUTCOME_CO_TRYX(t.value());

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
    assert(offset >= 0);
    auto data_bytes = iovs.bytes();
    assert(data_bytes > 0 && data_bytes % block_size == 0);

    // pin the current object to reserve space in the logobj, and while the
    // log is held check if we have to rollover
    auto lck = co_await logobj_mtx.co_scoped_lock();
    auto obj = cur_logobj;
    obj->write_start();
    auto [ext, buf] = obj->append(data_bytes);
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
    co_return outcome::success();
}

ResTask<void> LsvdImage::trim(off_t offset, usize len)
{
    assert(offset >= 0);
    if (len == 0)
        co_return outcome::success();

    auto ol = co_await logobj_mtx.co_scoped_lock();
    auto obj = cur_logobj;
    obj->write_start();
    auto [ext, buf] = obj->append(len);
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
    co_return outcome::success();
}

ResTask<void> LsvdImage::flush()
{
    auto ol = co_await logobj_mtx.co_scoped_lock();
    auto obj = co_await rollover_log(true);
    ol.unlock();

    co_await obj->wait_for_writes();
    co_return outcome::success();
}

// Assumes that the logobj_mtx is held exclusively
Task<sptr<LogObj>> LsvdImage::rollover_log(bool force)
{
    if (!force && cur_logobj->remaining() > rollover_threshold) {
        co_return cur_logobj;
    }

    auto prev = cur_logobj;
    auto new_seqnum = prev->seqnum + 1;

    // add new checkpoint if needed
    if (new_seqnum > prev->seqnum + checkpoint_interval_epoch) {
        auto ckpt_buf = co_await extmap->serialise();
        checkpoint(new_seqnum, std::move(ckpt_buf))
            .scheduleOn(co_await folly::coro::co_current_executor)
            .start();
        new_seqnum += 1;
    }

    auto new_logobj = std::make_shared<LogObj>(new_seqnum, max_log_size);
    {
        auto l = co_await pending_mtx.co_scoped_lock();
        auto [it, inserted] = pending_objs.emplace(new_seqnum, new_logobj);
        assert(inserted == true);
    }

    cur_logobj = new_logobj;
    prev->mark_complete();

    auto exe = co_await folly::coro::co_current_executor;
    flush_logobj(prev).scheduleOn(exe).start();
    co_return prev;
}

ResTask<void> LsvdImage::flush_logobj(sptr<LogObj> obj)
{
    co_await obj->wait_for_writes();
    auto k = get_logobj_key(name, obj->seqnum);

    // TODO think about what to do in the case of failure here
    BOOST_OUTCOME_CO_TRYX(co_await s3->write(k, obj->as_iov()));
    BOOST_OUTCOME_CO_TRYX(
        co_await cache->insert_obj(obj->seqnum, obj->as_buffer()));

    auto l = co_await pending_mtx.co_scoped_lock();
    pending_objs.erase(obj->seqnum);

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
    u32 consumed_bytes = start_byte;
    while (consumed_bytes < buf.size()) {
        auto entry = reinterpret_cast<log_entry *>(buf.data() + consumed_bytes);
        switch (entry->type) {
        case log_entry_type::WRITE:
            co_await extmap->update(entry->offset, entry->len,
                                    S3Ext{seq, consumed_bytes, entry->len});
            break;
        case log_entry_type::TRIM:
            co_await extmap->unmap(entry->offset, entry->len);
            break;
        default:
            XLOG(ERR, "Unknown log entry type {}",
                 static_cast<u32>(entry->type));
            co_return outcome::failure(std::errc::io_error);
        }
        consumed_bytes += sizeof(log_entry) + entry->len;
    }

    co_return outcome::success();
}

ResTask<uptr<LsvdImage>> LsvdImage::mount(sptr<ObjStore> s3, fstr name,
                                          fstr cfg_str)
{
    XLOG(INFO, "Mounting {} with config {}", name, cfg_str);
    auto img = std::unique_ptr<LsvdImage>(new LsvdImage(name));

    img->cfg = BOOST_OUTCOME_CO_TRYX(LsvdConfig::parse(cfg_str));

    SuperblockInfo superblock;
    auto super_buf = BOOST_OUTCOME_CO_TRYX(co_await s3->read_all(name));
    BOOST_OUTCOME_CO_TRYX(superblock.deserialise(super_buf));
    img->superblock = superblock;

    XLOG(INFO, "Found checkpoints: {}",
         folly::join(", ", superblock.checkpoints));
    img->last_checkpoint = superblock.checkpoints.back();
    auto last_ckpt_key = get_logobj_key(name, img->last_checkpoint);
    auto last_ckpt_buf =
        BOOST_OUTCOME_CO_TRYX(co_await s3->read_all(last_ckpt_key));
    img->extmap = ExtMap::deserialise(last_ckpt_buf);

    img->s3 = s3;
    img->cache = make_image_cache(s3, name);
    img->journal =
        BOOST_OUTCOME_CO_TRYX(co_await Journal::open(img->cfg.journal_path));

    // recover the image starting from the last checkpoint
    u32 err_count = 0;
    for (auto cur_seq = img->last_checkpoint + 1;
         err_count < LOG_REPLAY_OBJECT_COUNT; cur_seq++) {
        auto lo_buf = co_await s3->read_all(get_logobj_key(name, cur_seq));
        if (lo_buf.has_value())
            BOOST_OUTCOME_CO_TRYX(
                co_await img->replay_obj(cur_seq, lo_buf.value(), 0));
        else
            err_count++;
    }

    // TODO search for uncommited objects in the journal and replay them too

    co_return img;
}

Task<void> LsvdImage::unmount()
{
    auto ol = co_await logobj_mtx.co_scoped_lock();
    auto obj = co_await rollover_log(true);
    co_await obj->wait_for_writes();
}

ResTask<void> LsvdImage::create(sptr<ObjStore> s3, fstr name, usize size)
{
    XLOG(INFO, "Creating new image {}", name);

    XLOG(DBG, "Writing out checkpoint of empty extmap");
    ExtMap extmap;
    auto extmap_buf = co_await extmap.serialise();
    auto ckpt_key = get_logobj_key(name, 1);
    BOOST_OUTCOME_CO_TRYX(co_await s3->write(
        ckpt_key, iovec{extmap_buf.data(), extmap_buf.size()}));

    XLOG(DBG, "Creating superblock for image of size {}", size);
    auto superblock = SuperblockInfo{
        .image_size = size, .clones = {}, .checkpoints = {1}, .snapshots = {}};
    auto super_buf = BOOST_OUTCOME_CO_TRYX(superblock.serialise());
    BOOST_OUTCOME_CO_TRYX(
        co_await s3->write(name, iovec{super_buf.data(), super_buf.size()}));

    co_return outcome::success();
}

ResTask<void> LsvdImage::remove(sptr<ObjStore> s3, fstr name)
{
    XLOG(INFO, "Removing image {}", name);
    // TODO
    co_return outcome::failure(std::errc::not_supported);
}

ResTask<void> LsvdImage::clone(sptr<ObjStore> s3, fstr src, fstr dst)
{
    XLOG(INFO, "Cloning image {} to {}", src, dst);
    // TODO
    co_return outcome::failure(std::errc::not_supported);
}
