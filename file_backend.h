/*
 * file:        file_backend.h
 * description: local filesystem-based object backend 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef FILE_BACKEND_H
#define FILE_BACKEND_H

#include <libaio.h>
#include <thread>
#include "backend.h"

/* nevermind separating interface and implementation - this
 * is so simple...
 */
class file_backend : public backend {
    bool e_io_running = false;
    io_context_t ioctx;
    std::thread e_io_th;

public:
    file_backend();
    ~file_backend();

    /* see backend.h 
     */
    int write_object(const char *name, iovec *iov, int iovcnt);
    int read_object(const char *name, iovec *iov, int iovcnt, size_t offset);
    int delete_object(const char *name);
    
    /* async I/O
     */
    request *make_write_req(const char *name, iovec *iov, int iovcnt);
    request *make_read_req(const char *name, size_t offset,
                           iovec *iov, int iovcnt);
    request *make_read_req(const char *name, size_t offset,
                           char *buf, size_t len);
};

#endif
