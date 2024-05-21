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

    virtual int write(std::string name, smartiov &iov) = 0;
    virtual int read(std::string name, off_t offset, smartiov &iov) = 0;
    virtual int delete_obj(std::string name) = 0;

    virtual request *aio_write(std::string name, smartiov &iov) = 0;
    virtual request *aio_read(std::string name, off_t offset,
                              smartiov &iov) = 0;
    virtual request *aio_delete(std::string name) = 0;

    int write(std::string name, void *buf, size_t len)
    {
        smartiov iov((char *)buf, len);
        return write(name, iov);
    }

    int read(std::string name, off_t offset, void *buf, size_t len)
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

    virtual opt<u64> get_size(std::string name) = 0;
    virtual opt<vec<byte>> read_whole_obj(std::string name) = 0;
    virtual bool exists(std::string name) = 0;
};

extern std::shared_ptr<backend> make_file_backend(const char *prefix);
extern std::shared_ptr<backend> make_rados_backend(rados_ioctx_t io);
