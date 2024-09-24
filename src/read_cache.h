#pragma once
#include "backend.h"
#include "representation.h"
#include "smartiov.h"
#include "utils.h"

class ReadCache
{
  public:
    virtual ~ReadCache() {}

    virtual ResTask<void> read(S3Ext ext, smartiov &dest) = 0;
    virtual ResTask<void> insert_obj(seqnum_t seqnum, buffer iov) = 0;
};

uptr<ReadCache> make_image_cache(sptr<ObjStore> s3, fstr imgname);
