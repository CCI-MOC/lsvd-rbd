#pragma once

#include "lsvd_types.h"
#include "smartiov.h"
#include "utils.h"

class read_cache_coro
{
  public:
    read_cache_coro() {}
    virtual ~read_cache_coro() {}

    virtual bool should_bypass_cache(str img, seqnum_t seqnum,
                                     usize offset) = 0;
    virtual void served_bypass_request(str img, seqnum_t seqnum, usize offset,
                                       usize bytes) = 0;
    virtual ResTask<void> read(str img, seqnum_t seqnum, usize offset,
                               usize adjust, smartiov &dest) = 0;
    virtual ResTask<void> insert_object(str img, seqnum_t seqnum, iovec v) = 0;
};
