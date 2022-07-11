/*
io.h : include file containing all of the main io functions used by the lsvd system 
including a base structure for io: e_iocb
*/

#ifndef IO_H
#define IO_H

#include "lsvd_includes.h"
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

size_t getsize64(int fd);

void e_iocb_cb(io_context_t ctx, iocb *io, long res, long res2);

struct e_iocb {
    iocb io;
    void (*cb)(void*) = NULL;
    void *ptr = NULL;
    e_iocb() { io_set_callback(&io, e_iocb_cb); }
};

int io_queue_wait(io_context_t ctx, struct timespec *timeout);

void e_iocb_runner(io_context_t ctx, bool *running, const char *name);

void e_io_prep_pwrite(e_iocb *io, int fd, void *buf, size_t len, size_t offset,
		      void (*cb)(void*), void *arg);

void e_io_prep_pread(e_iocb *io, int fd, void *buf, size_t len, size_t offset,
		     void (*cb)(void*), void *arg);

void e_io_prep_pwritev(e_iocb *io, int fd, const struct iovec *iov, int iovcnt,
		     size_t offset, void (*cb)(void*), void *arg);

void e_io_prep_preadv(e_iocb *eio, int fd, const struct iovec *iov, int iovcnt,
		    size_t offset, void (*cb)(void*), void *arg);

int e_io_submit(io_context_t ctx, e_iocb *eio);

#endif
