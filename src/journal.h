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
    static TaskRes<uptr<Journal>> open(fstr path);

    TaskUnit record_write(off_t offset, iovec iov, S3Ext ext);
    TaskUnit record_trim(off_t offset, usize len, S3Ext ext);
};