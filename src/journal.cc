#include "journal.h"

TaskRes<uptr<Journal>> Journal::open(fstr path)
{
    todo();
    co_return nullptr;
}

TaskUnit Journal::record_write(off_t offset, iovec iov, S3Ext ext)
{
    // todo();
    co_return folly::Unit();
}

TaskUnit Journal::record_trim(off_t offset, usize len, S3Ext ext)
{
    // todo();
    co_return folly::Unit();
}
