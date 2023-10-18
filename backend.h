/* This file is the header file for the backend for lsvd which simply uses
 * definitions from sys/uio.h in order to have base functions for write and read
 * operations for IO
 */
#pragma once

#include <memory>
#include <string>
#include <sys/uio.h>

#include "fake_rbd.h"

class request;

class backend
{
  public:
    virtual ~backend() {}

    /* synchronous I/O methods, return 0 / -1 for success/error
     */
    virtual int write_object(const char *name, iovec *iov, int iovcnt) = 0;
    virtual int write_object(const char *name, char *buf, size_t len) = 0;
    virtual int read_object(const char *name, iovec *iov, int iovcnt,
                            size_t offset) = 0;
    virtual int read_object(const char *name, char *buf, size_t len,
                            size_t offset) = 0;
    virtual int delete_object(const char *name) = 0;
    virtual request *delete_object_req(const char *name) = 0;

    /* async I/O
     */
    virtual std::unique_ptr<request> make_write_req(const char *name,
                                                    iovec *iov, int iovcnt) = 0;
    virtual std::unique_ptr<request> make_write_req(const char *name, char *buf,
                                                    size_t len) = 0;
    virtual std::unique_ptr<request>
    make_read_req(const char *name, size_t offset, iovec *iov, int iovcnt) = 0;
    virtual std::unique_ptr<request>
    make_read_req(const char *name, size_t offset, char *buf, size_t len) = 0;
};

extern std::unique_ptr<backend> make_file_backend(const char *prefix);
extern std::unique_ptr<backend> make_rados_backend(rados_ioctx_t io);
