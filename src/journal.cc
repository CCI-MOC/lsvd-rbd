#include "journal.h"

ResTask<uptr<Journal>> Journal::open(fstr path)
{
    todo();
    co_return outcome::success();
}

ResTask<void> Journal::record_write(off_t offset, iovec iov, S3Ext ext)
{
    // todo();
    co_return outcome::success();
}

ResTask<void> Journal::record_trim(off_t offset, usize len, S3Ext ext)
{
    todo();
    co_return outcome::success();
}
