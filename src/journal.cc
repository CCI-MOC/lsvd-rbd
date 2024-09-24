#include "journal.h"

ResTask<uptr<Journal>> Journal::open(fstr path)
{
    // TODO temporarily noops
    XLOG(ERR, "Journal::open not implemented");
    co_return outcome::failure(std::errc::not_supported);
}

ResTask<void> Journal::record_write(off_t offset, iovec iov, S3Ext ext)
{
    // TODO temporarily noops
    co_return outcome::success();
}

ResTask<void> Journal::record_trim(off_t offset, usize len, S3Ext ext)
{
    // TODO temporarily noops
    co_return outcome::success();
}
