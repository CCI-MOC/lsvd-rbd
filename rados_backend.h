// file:	rados_backend.h
// description: This file contains all the rados_backend class functions. All functions used by LSVD
//		which focus on manipulating data to and from the rados backend
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

#ifndef RADOS_BACKEND_H
#define RADOS_BACKEND_H

std::pair<std::string,std::string> split_string(std::string s,
                                                std::string delim);

class rados_backend : public backend {
    std::mutex m;
    char *pool;
    char *prefix;
    rados_t cluster;
    rados_ioctx_t io_ctx;

    struct rados_aio {
        void (*cb)(void*);
        void *ptr;
        rados_completion_t c;
    };
    static void aio_read_done(rados_completion_t c, void *ptr);

public:
    rados_backend(const char *_prefix);
    ssize_t write_object(const char *name, iovec *iov, int iovcnt);
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt);
    void delete_numbered_object(int seq);
    ssize_t read_object(const char *name, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_object(int seq, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset);

    int aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
                        void (*cb)(void*), void *ptr);
    int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
                                  void (*cb)(void*), void *ptr);
    ~rados_backend();

    std::string object_name(int seq);
};

#endif

