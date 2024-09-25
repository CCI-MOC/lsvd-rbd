#pragma once
#include "backend.h"
#include "representation.h"
#include "smartiov.h"
#include "utils.h"

class Journal
{
  private:
    sptr<FileIo> journ_file;
    usize journ_size;

  public:
    static ResTask<uptr<Journal>> open(fstr path);

    ResTask<void> record_write(off_t offset, iovec iov, S3Ext ext);
    ResTask<void> record_trim(off_t offset, usize len, S3Ext ext);
};