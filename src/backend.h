#pragma once

#include <memory>
#include <rados/librados.h>
#include <sys/uio.h>

#include "request.h"
#include "smartiov.h"
#include "utils.h"

class backend
{
  public:
    virtual ~backend() {}

    virtual Result<int> write(std::string name, smartiov &iov) = 0;
    virtual Result<int> read(std::string name, off_t offset, smartiov &iov) = 0;
    virtual Result<void> delete_obj(std::string name) = 0;

    virtual request *aio_write(std::string name, smartiov &iov) = 0;
    virtual request *aio_read(std::string name, off_t offset,
                              smartiov &iov) = 0;
    virtual request *aio_delete(std::string name) = 0;

    Result<int> write(std::string name, void *buf, size_t len)
    {
        smartiov iov((char *)buf, len);
        return write(name, iov);
    }

    Result<int> read(std::string name, off_t offset, void *buf, size_t len)
    {
        smartiov iov((char *)buf, len);
        return read(name, offset, iov);
    }

    request *aio_write(std::string name, void *buf, size_t len)
    {
        smartiov iov((char *)buf, len);
        return aio_write(name, iov);
    }

    request *aio_read(std::string name, off_t offset, void *buf, size_t len)
    {
        smartiov iov((char *)buf, len);
        return aio_read(name, offset, iov);
    }

    virtual Result<u64> get_size(std::string name) = 0;
    virtual Result<vec<byte>> read_whole_obj(std::string name) = 0;
    virtual bool exists(std::string name) = 0;
};

extern std::shared_ptr<backend> make_rados_backend(rados_ioctx_t io);

class backend_coro
{
  public:
    virtual ~backend_coro() {}

    virtual ResTask<usize> get_size(str name) = 0;
    virtual ResTask<bool> exists(str name) = 0;

    virtual ResTask<usize> read(str name, usize offset, smartiov &iov) = 0;
    virtual ResTask<usize> read(str name, usize offset, iovec iov) = 0;
    virtual ResTask<vec<byte>> read_all(str name) = 0;
    virtual ResTask<usize> write(str name, smartiov &iov) = 0;
    virtual ResTask<usize> write(str name, iovec iov) = 0;
};

sptr<backend_coro> make_coro_backend(rados_ioctx_t io);

Result<rados_ioctx_t> connect_to_pool(str pool_name);
