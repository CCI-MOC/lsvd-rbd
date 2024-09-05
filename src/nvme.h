#pragma once

#include "request.h"
#include "smartiov.h"

class nvme
{
  public:
    virtual ~nvme() = 0;

    virtual int read(void *buf, size_t count, off_t offset) = 0;
    virtual int write(const void *buf, size_t count, off_t offset) = 0;

    virtual int writev(const struct iovec *iov, int iovcnt, off_t offset) = 0;
    virtual int readv(const struct iovec *iov, int iovcnt, off_t offset) = 0;

    virtual request *make_write_request(smartiov *iov, size_t offset) = 0;
    virtual request *make_write_request(char *buf, size_t len,
                                        size_t offset) = 0;
    virtual request *make_read_request(smartiov *iov, size_t offset) = 0;

    virtual request *make_read_request(char *buf, size_t len,
                                       size_t offset) = 0;
};

class fileio
{
  public:
    virtual ~fileio() = 0;

    virtual ResTask<usize> read(usize offset, smartiov &iov) = 0;
    virtual ResTask<usize> read(usize offset, iovec iov) = 0;
    virtual ResTask<usize> write(usize offset, smartiov &iov) = 0;
    virtual ResTask<usize> write(usize offset, iovec iov) = 0;
};

/**
Takes ownership of the fd, will close it on destruction.
 */
uptr<fileio> make_fileio(int fd);

nvme *make_nvme_uring(int fd, const char *name);
