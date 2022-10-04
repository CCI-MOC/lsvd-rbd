/*
 * file:        request.h
 * description: generic inter-layer request mechanism
 * 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef REQUEST_H
#define REQUEST_H

#include "lsvd_types.h"
#include "smartiov.h"

/* generic interface for requests.
 *  - run(parent): begin execution
 *  - notify(rv): notification of completion
 *  - TODO: wait(): wait for completion
 */
class request {
public:
    virtual sector_t lba() = 0;
    virtual smartiov *iovs() = 0;
    
    virtual bool is_done() = 0;
    virtual void wait() = 0;
    virtual void run(request *parent) = 0;
    virtual void notify(request *child) = 0;
    virtual void release() = 0;
    virtual ~request(){}
    request() {}
};

/* total hack, for converting current code based on callbacks
 */
class callback_req : public request {
    void (*cb)(void*);
    void *ptr;

public:
    callback_req(void (*cb_)(void*), void *ptr_) : cb(cb_), ptr(ptr_) {}
    ~callback_req() {}
    void run(request *parent) {}
    void notify(request *unused) {
	cb(ptr);
	delete this;
    }
    sector_t lba() { return 0;}
    smartiov *iovs() { return NULL; }
    bool is_done() { return false; }
    void release() {}
    void wait() {}
};

#endif
