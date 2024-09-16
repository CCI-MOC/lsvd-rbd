#pragma once

#include "representation.h"
#include "smartiov.h"
#include "utils.h"

class Journal
{

  public:
    static uptr<Journal> open(fspath path);

    Task<void> backpressure();
    ResTask<void> record_write(off_t offset, iovec iov, S3Ext ext);
    ResTask<void> record_trim(off_t offset, usize len, S3Ext ext);
};