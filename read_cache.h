/*
 * file:        read_cache.h
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

struct j_read_super;
#include "config.h"
#include "extent.h"
#include "smartiov.h"

class translate;

class read_cache
{
  public:
    virtual ~read_cache(){};

    virtual void
    handle_read(size_t offset, smartiov *iovs,
                std::vector<sptr<request>> &requests) = 0;

    virtual void write_map(void) = 0;
};

extern std::unique_ptr<read_cache>
make_read_cache(uint32_t blkno, int _fd, translate *_be, lsvd_config *cfg,
                extmap::objmap *map, extmap::bufmap *bufmap,
                std::shared_mutex *m, std::mutex *bufmap_m, backend *_io);

#endif
