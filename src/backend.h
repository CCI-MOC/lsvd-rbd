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

    virtual TaskRes<usize> get_size(fstr name) = 0;

    virtual TaskRes<vec<byte>> read_all(fstr name) = 0;
    virtual TaskRes<usize> read(fstr name, off_t offset, smartiov &iov) = 0;
    virtual TaskRes<usize> read(fstr name, off_t offset, iovec iov) = 0;
    virtual TaskRes<usize> write(fstr name, smartiov &iov) = 0;
    virtual TaskRes<usize> write(fstr name, iovec iov) = 0;
    virtual TaskUnit remove(fstr name) = 0;

    static Result<sptr<ObjStore>> connect_to_pool(fstr pool_name);
};

class FileIo
{
    virtual TaskUnit preadv(off_t offset, smartiov iov) = 0;
    virtual TaskUnit pwritev(off_t offset, smartiov iov) = 0;
};
