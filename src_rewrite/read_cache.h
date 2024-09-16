#pragma once

#include "representation.h"
#include "smartiov.h"
#include "utils.h"

class ReadCache
{
  public:
    virtual ~ReadCache() {}

    virtual bool should_bypass_cache(str img, seqnum_t seqnum,
                                     usize offset) = 0;
    virtual void served_bypass_request(str img, seqnum_t seqnum, usize offset,
                                       usize bytes) = 0;
    virtual ResTask<void> read(S3Ext ext, smartiov &dest) = 0;
    virtual ResTask<void> insert_object(str img, seqnum_t seqnum, iovec v) = 0;
};
