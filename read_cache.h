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

#include <stddef.h>
#include <stdint.h>

#include <vector>
#include <map>

struct j_read_super;
#include "extent.h"
#include "config.h"
#include "image.h"

class read_cache {
public:

    virtual ~read_cache() {};

    virtual void handle_read(rbd_image *img,
			     size_t offset, smartiov *iovs,
			     std::vector<request*> &requests) = 0;

    virtual void write_map(void) = 0;
};

extern read_cache *make_read_cache(uint32_t blkno, int _fd, bool nt,
                                   translate *_be, extmap::objmap *map,
                                   std::shared_mutex *m, backend *_io);

#endif

