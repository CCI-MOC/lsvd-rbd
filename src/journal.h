#pragma once
#include <folly/experimental/coro/SharedMutex.h>

#include "backend.h"
#include "representation.h"
#include "smartiov.h"
#include "utils.h"

class Journal
{
  private:
    s32 fd; // we own this fd
    sptr<FileIo> journ_io;
    usize journ_size;

    folly::coro::SharedMutex mtx;
    std::atomic<usize> cur_offset;

    Journal(s32 fd, sptr<FileIo> journ_io, usize journ_size);

  public:
    ~Journal();
    static Result<uptr<Journal>> open(fspath path, usize size);

    TaskUnit record_write(off_t offset, iovec iov, S3Ext ext);
    TaskUnit record_trim(off_t offset, usize len, S3Ext ext);
};