#pragma once

#include <atomic>
#include <folly/container/F14Map.h>
#include <folly/experimental/coro/SharedMutex.h>

#include "representation.h"
#include "smartiov.h"
#include "utils.h"

/**

A wrapper around the "log-structured" part of LSVD. Owns the objects that will
be written to the backend and associated functions like backend flushes.

 */
class LsvdLog
{
    const usize ROLLOVER_THRESHOLD = 4 * 1024 * 1024;  // 4MB
    const usize MAX_LOG_SIZE = ROLLOVER_THRESHOLD * 2; // 8MB

    struct LogObj {
        const seqnum_t seqnum;
        std::atomic<int> read_refs;
        std::atomic<int> write_refs;
        vec<byte> data;
    };

    folly::coro::SharedMutex mtx;
    LogObj &cur_obj;
    std::atomic<seqnum_t> cur_seqnum;
    std::atomic<usize> cur_obj_offset;

    folly::F14FastMap<seqnum_t, LogObj> pending_flush;

  public:
    struct LogHandle {
        S3Ext ext;
        LogObj &obj;

        LogHandle(S3Ext ext, byte *ptr, LogObj &obj) : ext(ext), obj(obj)
        {
            obj.write_refs.fetch_add(1, std::memory_order_seq_cst);
        }

        ~LogHandle() { obj.write_refs.fetch_sub(1, std::memory_order_seq_cst); }
    };

    /**
    Tries to read a range of data from objects that are pending flush. If
    successful, copy the data into the iovs and return true, otherwise return
    false and do nothing else.
     */
    Task<bool> try_read(S3Ext ext, smartiov &iovs);

    /**
    Allocates the requested space for a new log entry. This does NOT fill it
    with any kind of metadata; the caller is responsible for requesting some
    additional space for metadata headers and filling them in.
     */
    Task<LogHandle> new_entry(usize len);

    /**
    Record a write on the log. This will fill in the metadata as appropriate and
    return a loghandle and a ptr to the buffer where it should be copied, but
    the extent is NOT shrunk to ONLY include the data and not the metadata
    header, it includes both.
    */
    Task<std::pair<LogHandle, byte *>> new_write_entry(off_t offset, usize len);

    /**
    Record a write on the log. This will fill in the metadata as appropriate
    and return a loghandle to the entire entry, including the metadata.
     */
    Task<LogHandle> new_trim_entry(off_t offset, usize len);

    Task<LogObj &> force_rollover();
};