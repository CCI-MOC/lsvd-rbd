// file:	translate.h
// description: this class focuses on the implementation of the translation layer to the system with
//              an object oriented approach. This file contains the following which things which are
//              documented below:
//                      -batch structure
//                      -translate class
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

#ifndef TRANSLATE_H
#define TRANSLATE_H

#include <sys/uio.h>            /* iovec */

class translate {
public:
    translate() {}
    virtual ~translate() {}

    virtual ssize_t init(const char *name, int nthreads, bool timedflush) = 0;
    virtual void shutdown(void) = 0;

    virtual int flush(void) = 0;      /* write out current batch */
    virtual int checkpoint(void) = 0; /* flush, then write checkpoint */

    virtual ssize_t writev(size_t offset, iovec *iov, int iovcnt) = 0;
    virtual ssize_t readv(size_t offset, iovec *iov, int iovcnt) = 0;

    virtual const char *prefix() = 0; /* for read cache */
    
    /* debug functions
     */
    virtual void getmap(int base, int limit,
                        int (*cb)(void *ptr,int,int,int,int), void *ptr) = 0;
    virtual int mapsize(void) = 0;
    virtual void reset(void) = 0;
    virtual int frontier(void) = 0;
};

extern translate *make_translate(backend *_io, objmap *omap);

#endif
