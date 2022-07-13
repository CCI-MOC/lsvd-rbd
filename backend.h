#ifndef BACKEND_H
#define BACKEND_H

#include <sys/uio.h>
#include <string>

class backend {
public:
    virtual ssize_t write_object(const char *name, iovec *iov, int iovcnt) = 0;
    virtual ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) = 0;
    virtual ssize_t read_object(const char *name, char *buf, size_t len, size_t offset) = 0;
    virtual ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt,
                                          size_t offset) = 0;
    virtual void    delete_numbered_object(int seq) = 0;
    virtual ssize_t read_numbered_object(int seq, char *buf, size_t len,
                                         size_t offset) = 0;
    virtual int aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
                                    void (*cb)(void*), void *ptr) = 0;
    virtual int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
                                          void (*cb)(void*), void *ptr) = 0;
    virtual std::string object_name(int seq) = 0;
    virtual ~backend(){}
};

#endif
