#include <boost/outcome/try.hpp>

#include "image.h"
#include "representation.h"
#include "smartiov.h"

FutRes<void> LsvdImage::read(off_t offset, smartiov iovs)
{
    return read_task(offset, std::move(iovs)).semi().via(executor);
}

FutRes<void> LsvdImage::write(off_t offset, smartiov iovs)
{
    return write_task(offset, std::move(iovs)).semi().via(executor);
}

ResTask<void> LsvdImage::read_task(off_t offset, smartiov iovs)
{
    // Since backend objects are immutable, we just need a read lock for as long
    // as we're getting the extents. Once we have the extents, the map can be
    // moified and we don't care.
    auto exts = co_await extmap.lookup(offset, iovs.bytes());

    folly::fbvector<folly::SemiFuture<Result<void>>> tasks(exts.size());
    for (auto &[img_off, ext] : exts) {
        auto base = img_off - offset;

        // Unmapped range; zero it and move on
        if (ext.seqnum == 0) {
            iovs.zero(base, base + ext.len);
            continue;
        }

        auto ext_iov = iovs.slice(base, ext.len);

        // First try to read from the write log in case it's in memory
        auto success = co_await wlog->try_read(ext, ext_iov);
        if (success)
            continue;

        // Not in pending logobjs, get it from the read-through backend cache
        tasks.push_back(cache->read(ext, ext_iov).semi());
    }

    auto all = co_await folly::collectAll(tasks);
    for (auto &t : all)
        if (t.hasException())
            co_return std::errc::io_error;
        else if (t.value().has_error())
            co_return t.value().error();

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
ResTask<void> LsvdImage::write_task(off_t offset, smartiov iovs)
{
    auto data_bytes = iovs.bytes();
    assert(data_bytes > 0 && data_bytes % block_size == 0);

    auto [h, buf] = co_await wlog->new_write_entry(offset, data_bytes);

    // we can't avoid the copy, we have to retain the write somewhere until
    // we can flush it to the backend
    iovs.copy_out(buf);

    BOOST_OUTCOME_CO_TRY(
        co_await journal->record_write(offset, iovec{buf, data_bytes}, h.ext));

    // Update the extent map and ack. The extent map takes extents that do not
    // include headers, so strip those off before updating.
    auto data_ext = get_data_ext(h.ext);
    co_await extmap.update(offset, data_bytes, data_ext);

    co_return outcome::success();
}

ResTask<void> LsvdImage::trim_task(off_t offset, usize len)
{
    auto h = co_await wlog->new_trim_entry(offset, len);
    BOOST_OUTCOME_CO_TRY(co_await journal->record_trim(offset, len, h.ext));
    co_await extmap.unmap(offset, len);
    co_return outcome::success();
}

ResTask<void> LsvdImage::flush_task()
{
    co_await wlog->force_rollover();
    co_return outcome::success();
}
