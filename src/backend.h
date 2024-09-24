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

    virtual ResTask<usize> get_size(fstr name) = 0;
    virtual ResTask<bool> exists(fstr name) = 0;

    virtual ResTask<vec<byte>> read_all(fstr name) = 0;
    virtual ResTask<usize> read(fstr name, off_t offset, smartiov &iov) = 0;
    virtual ResTask<usize> read(fstr name, off_t offset, iovec iov) = 0;
    virtual ResTask<usize> write(fstr name, smartiov &iov) = 0;
    virtual ResTask<usize> write(fstr name, iovec iov) = 0;
    virtual ResTask<void> remove(fstr name) = 0;

    static Result<uptr<ObjStore>> connect_to_pool(fstr pool_name);
};

class FileIo
{
    virtual ResTask<void> preadv(off_t offset, smartiov iov) = 0;
    virtual ResTask<void> pwritev(off_t offset, smartiov iov) = 0;
};
