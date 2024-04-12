#pragma once

#include <shared_mutex>
#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "config.h"
#include "extent.h"
#include "shared_read_cache.h"
#include "smartiov.h"
#include "utils.h"

class translate;

class img_reader
{
  public:
    virtual ~img_reader(){};

    virtual void handle_read(size_t offset, smartiov *iovs,
                             std::vector<request *> &requests) = 0;
};

uptr<img_reader> make_reader(uint32_t blkno, translate *_be, lsvd_config *cfg,
                             extmap::objmap *map, extmap::bufmap *bufmap,
                             std::shared_mutex *m, std::mutex *bufmap_m,
                             sptr<backend> _io, sptr<read_cache> shared_cache);
