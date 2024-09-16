#include "writelog.h"

Task<bool> LsvdLog::try_read(S3Ext ext, smartiov &iovs)
{
    auto l = co_await mtx.co_scoped_lock_shared();

    auto it = pending_flush.find(ext.seqnum);
    if (it == pending_flush.end()) {
        co_return false;
    }

    auto &obj = it->second;
    obj.read_refs += 1;
    l.unlock();

    iovs.copy_in(obj.data.data() + ext.offset, ext.len);
    obj.read_refs -= 1;
    co_return true;
}

Task<LsvdLog::LogHandle> LsvdLog::new_entry(usize len) {}
