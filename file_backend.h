// file:	file_backend.h
// description: File_backend is a class which hosts functions for specifically writing objects to files and managing
//		data within files instead of the disk.
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

#ifndef FILE_BACKEND_H
#define FILE_BACKEND_H

class file_backend : public backend {
    char *prefix;
    bool e_io_running = false;
    io_context_t ioctx;
    std::thread e_io_th;

public:
    file_backend(const char *_prefix);
    ~file_backend();

    /* see backend.h for description
     */ 
    ssize_t write_object(const char *name, iovec *iov, int iovcnt);
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt);
    void delete_numbered_object(int seq);
    ssize_t read_object(const char *name, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_object(int seq, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset);
    int aio_read_num_object(int seq, char *buf, size_t len,
			    size_t offset, void (*cb)(void*), void *ptr);
    int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
				  void (*cb)(void*), void *ptr);
    std::string object_name(int seq);
};

#endif
