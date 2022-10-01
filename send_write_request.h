/*
 * file:        send_write_request.h
 * description: write logic for write_cache 
 * 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef SEND_WRITE_REQUEST_H
#define SEND_WRITE_REQUEST_H

class write_cache_impl;

class send_write_request : public request {
    std::atomic<int> reqs = 0;

    sector_t      plba;
    std::vector<request*> *work = NULL;
    request      *r_data = NULL;
    char         *hdr = NULL;
    smartiov     *data_iovs = NULL;

    request      *r_pad = NULL;
    char         *pad_hdr = NULL;
    smartiov     *pad_iov = NULL;

    write_cache_impl *wcache = NULL;
    
public:
    send_write_request(std::vector<request*> *w, page_t n_pages, page_t page,
                       page_t n_pad, page_t pad,
                       write_cache *wcache);
    ~send_write_request();

    sector_t lba();
    smartiov *iovs();
    bool is_done(void);
    void wait();
    void run(request *parent);
    void notify(request *child);
    void release();
};

#endif
