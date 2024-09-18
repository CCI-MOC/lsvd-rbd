#pragma once

#include <folly/File.h>
#include <rados/librados.h>
#include <sys/uio.h>

#include "representation.h"
#include "smartiov.h"
#include "utils.h"

class ObjStore
{
  public:
    virtual ~ObjStore() {}

    virtual ResTask<usize> get_size(str name) = 0;
    virtual ResTask<bool> exists(str name) = 0;

    virtual ResTask<vec<byte>> read_all(str name) = 0;
    virtual ResTask<usize> read(str name, off_t offset, smartiov &iov) = 0;
    virtual ResTask<usize> read(str name, off_t offset, iovec iov) = 0;
    virtual ResTask<usize> write(str name, smartiov &iov) = 0;
    virtual ResTask<usize> write(str name, iovec iov) = 0;
    virtual ResTask<void> remove(str name) = 0;
};

class FileIo
{
    virtual ResTask<void> preadv(off_t offset, smartiov iov) = 0;
    virtual ResTask<void> pwritev(off_t offset, smartiov iov) = 0;
};

Result<uptr<ObjStore>> connect_to_pool(str pool_name);
