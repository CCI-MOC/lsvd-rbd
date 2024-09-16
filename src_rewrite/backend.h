#pragma once

#include <rados/librados.h>
#include <sys/uio.h>

#include "representation.h"
#include "smartiov.h"
#include "utils.h"

class Backend
{
  public:
    virtual ~Backend() {}

    virtual ResTask<usize> get_size(str name) = 0;
    virtual ResTask<bool> exists(str name) = 0;

    virtual ResTask<usize> read(str name, off_t offset, smartiov &iov) = 0;
    virtual ResTask<usize> read(str name, off_t offset, iovec iov) = 0;
    virtual ResTask<vec<byte>> read_all(str name) = 0;
    virtual ResTask<usize> write(str name, smartiov &iov) = 0;
    virtual ResTask<usize> write(str name, iovec iov) = 0;
};

sptr<Backend> make_rados_backend(rados_ioctx_t io);
Result<rados_ioctx_t> connect_to_pool(str pool_name);
