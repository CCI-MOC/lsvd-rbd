// file:	io.h
// description: include file containing all of the main io functions used by the lsvd system
//		including a base structure for io: e_iocb
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

#ifndef IO_H
#define IO_H

// getsize64 :  returns the size of the file
size_t getsize64(int fd);

// e_iocb_cb :	callback function for the e_iocb structure
void e_iocb_cb(io_context_t ctx, iocb *io, long res, long res2);

// e_iocb : 	support structure which uses iocb structure, with an extra callback and void pointer
struct e_iocb {
    iocb io;
    void (*cb)(void*) = NULL;
    void *ptr = NULL;
    e_iocb() { io_set_callback(&io, e_iocb_cb); }
};

// io_queue_wait :	returns number of io events read
int io_queue_wait(io_context_t ctx, struct timespec *timeout);

// e_iocb_runner :	attempts to read events and if successful, usleeps for 100
void e_iocb_runner(io_context_t ctx, bool *running, const char *name);

// e_io_prep_pwrite :	calls io_prep_pwrite and sets callback via io_set_callback
void e_io_prep_pwrite(e_iocb *io, int fd, void *buf, size_t len, size_t offset,
		      void (*cb)(void*), void *arg);

// e_io_prep_pread :	calls io_prep_read and sets callback via io_set_callback
void e_io_prep_pread(e_iocb *io, int fd, void *buf, size_t len, size_t offset,
		     void (*cb)(void*), void *arg);

// e_io_prep_pwritev :   calls io_prep_pwritev and sets callback via io_set_callback
void e_io_prep_pwritev(e_iocb *io, int fd, const struct iovec *iov, int iovcnt,
		     size_t offset, void (*cb)(void*), void *arg);

// e_io_prep_preadv :    calls io_prep_readv and sets callback via io_set_callback
void e_io_prep_preadv(e_iocb *eio, int fd, const struct iovec *iov, int iovcnt,
		    size_t offset, void (*cb)(void*), void *arg);

// returns io_submit value
int e_io_submit(io_context_t ctx, e_iocb *eio);

#endif
