#include <boost/outcome/try.hpp>
#include <folly/experimental/coro/Invoke.h>

#include "image.h"
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
    auto exts = co_await extmap.lookup(offset, iovs.bytes());

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
    co_await log_rollover(false);
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
    co_await extmap.update(offset, data_bytes, data_ext);

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
    co_await log_rollover(false);
    ol.unlock();

    // Write out the data
    auto *entry = reinterpret_cast<log_entry *>(buf);
    *entry = log_entry{
        .type = log_entry_type::TRIM,
        .offset = static_cast<u64>(offset),
        .len = len,
    };

    BOOST_OUTCOME_CO_TRY(co_await journal->record_trim(offset, len, ext));
    co_await extmap.unmap(offset, len);

    obj->write_end();
    co_return outcome::success();
}

ResTask<void> LsvdImage::flush()
{
    auto ol = co_await logobj_mtx.co_scoped_lock();
    auto obj = co_await log_rollover(true);
    ol.unlock();

    co_await obj->wait_for_writes();
    co_return outcome::success();
}

// Assumes that the logobj_mtx is held exclusively
Task<sptr<LogObj>> LsvdImage::log_rollover(bool force)
{
    if (!force && cur_logobj->remaining() > rollover_threshold) {
        co_return cur_logobj;
    }

    auto prev = cur_logobj;
    auto new_seqnum = prev->seqnum + 1;

    // add new checkpoint if needed
    if (new_seqnum > prev->seqnum + checkpoint_interval_epoch) {
        co_await checkpoint(new_seqnum);
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

Task<void> LsvdImage::flush_logobj(sptr<LogObj> obj)
{
    co_await obj->wait_for_writes();
    auto k = get_key(obj->seqnum);

    // TODO think about what to do in the case of failure here
    std::ignore = co_await s3->write(k, obj->as_iov());
    std::ignore = co_await cache->insert_obj(obj->seqnum, obj->as_iov());

    auto l = co_await pending_mtx.co_scoped_lock();
    pending_objs.erase(obj->seqnum);
}

Task<void> LsvdImage::checkpoint(seqnum_t seqnum)
{
    auto buf = co_await extmap.serialise();
    auto k = get_key(seqnum);
    auto f = folly::coro::co_invoke(
        [this, k](auto buf) -> Task<void> {
            // TODO what to do if the checkpoint fails?
            std::ignore =
                co_await s3->write(k, iovec{(void *)buf.data(), buf.size()});
        },
        std::move(buf));
    std::move(f).scheduleOn(co_await folly::coro::co_current_executor).start();
}
