/*
 * file:        send_write_request.cc
 * description: write logic for write_cache 
 * 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <unistd.h>
#include <sys/uio.h>

#include <uuid/uuid.h>

#include <atomic>
#include <condition_variable>
#include <thread>
#include <shared_mutex>
#include <mutex>

#include <vector>
#include <map>
#include <stack>
#include <queue>
#include <cassert>

#include <algorithm>

#include "base_functions.h"

#include "journal.h"
#include "smartiov.h"
#include "extent.h"
#include "misc_cache.h"
#include "backend.h"
#include "io.h"
#include "translate.h"
#include "request.h"
#include "nvme.h"
#include "write_cache.h"
#include "write_cache_impl.h"
#include "send_write_request.h"

extern uuid_t my_uuid;

static uint64_t sectors(request *req) {
    return req->iovs()->bytes() / 512;
}

void send_write_request::notify(request *child) {
    child->release();
    if(--reqs > 0)
	return;
    {
	std::unique_lock lk(wcache->m);
	std::vector<extmap::lba2lba> garbage; 
	auto _plba = plba;

	/* update the write cache forward and reverse maps
	 */
	for (auto _w : *work) {
	    wcache->map.update(_w->lba(), _w->lba() + sectors(_w),
			       _plba, &garbage);
	    wcache->rmap.update(_plba, _plba+sectors(_w), _w->lba());
	    _plba += sectors(_w);
	    wcache->map_dirty = true;
	}

	/* remove reverse mappings for old versions of data
	 */
	for (auto it = garbage.begin(); it != garbage.end(); it++) 
	    wcache->rmap.trim(it->s.base, it->s.base+it->s.len);
    }

    /* send data to backend, invoke callbacks, then clean up
     */
    for (auto _w : *work) {
	auto [iov, iovcnt] = _w->iovs()->c_iov();
	wcache->be->writev(_w->lba()*512, iov, iovcnt);
	_w->notify(NULL);	// don't release multiple times
    }

    /* we don't implement release or wait - just delete ourselves.
     */
    delete this;
}

send_write_request::~send_write_request() {
    free(hdr);
    if (pad_hdr) 
	free(pad_hdr);
    delete data_iovs;
    if (pad_iov)
	delete pad_iov;
    delete work;
}

/* n_pages: number of 4KB data pages (not counting header)
 * page:    page number to begin writing 
 * n_pad:   number of pages to skip (not counting header)
 * pad:     page number for pad entry (0 if none)
 */
send_write_request::send_write_request(std::vector<request*> *work_,
				       page_t n_pages, page_t page,
				       page_t n_pad, page_t pad,
				       write_cache* wcache_)  {
    wcache = (write_cache_impl*)wcache_;
    work = work_;
    
    if (pad != 0) {
	pad_hdr = (char*)aligned_alloc(512, 4096);
	wcache->mk_header(pad_hdr, LSVD_J_PAD, my_uuid, n_pad+1);
	pad_iov = new smartiov();
	pad_iov->push_back((iovec){pad_hdr, 4096});
	reqs++;
	r_pad = wcache->nvme_w->make_write_request(pad_iov, pad*4096L);
    }
  
    std::vector<j_extent> extents;
    for (auto _w : *work)
	extents.push_back((j_extent){(uint64_t)_w->lba(), sectors(_w)});
  
    hdr = (char*)aligned_alloc(512, 4096);
    j_hdr *j = wcache->mk_header(hdr, LSVD_J_DATA, my_uuid, 1+n_pages);

    j->extent_offset = sizeof(*j);
    size_t e_bytes = extents.size() * sizeof(j_extent);
    j->extent_len = e_bytes;
    memcpy((void*)(hdr + sizeof(*j)), (void*)extents.data(), e_bytes);

    plba = (page+1) * 8;
    data_iovs = new smartiov();
    data_iovs->push_back((iovec){hdr, 4096});
    for (auto _w : *work) {
	auto [iov, iovcnt] = _w->iovs()->c_iov();
	data_iovs->ingest(iov, iovcnt);
    }
    r_data = wcache->nvme_w->make_write_request(data_iovs, page*4096L);
}

void send_write_request::run(request *parent /* unused */) {
    reqs++;
    if(r_pad) {
	reqs++;
	r_pad->run(this);
    }
    r_data->run(this);
}

bool      send_write_request::is_done() {return true;}
void      send_write_request::wait() {}
sector_t  send_write_request::lba() {return 0;}
smartiov *send_write_request::iovs() {return NULL;}
void      send_write_request::release() {}


