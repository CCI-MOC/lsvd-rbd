/*
 * file:        img_reader.h
 * description: interface for read cache
 * author:      Peter Desnoyers, Northeastern University
 *              Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef READ_CACHE_H
#define READ_CACHE_H

#include <map>
#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "shared_read_cache.h"
#include "config.h"
#include "extent.h"
#include "smartiov.h"
#include "utils.h"

class translate;

class img_reader
{
  public:
    virtual ~img_reader(){};

    virtual void handle_read(size_t offset, smartiov iovs,
                             std::vector<request *> &requests) = 0;
};

extern img_reader *make_reader(uint32_t blkno, translate *_be,
                               lsvd_config *cfg, extmap::objmap *map,
                               extmap::bufmap *bufmap, std::shared_mutex *m,
                               std::mutex *bufmap_m, sptr<backend> _io,
                               sptr<shared_read_cache> shared_cache);

#endif
