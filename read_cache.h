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

#include "translate.h"

class read_cache {
public:

    virtual ~read_cache() {};
    
    virtual std::pair<size_t,size_t>
        async_read(size_t offset, char *buf, size_t len,
                   void (*cb)(void*), void *ptr) = 0;

    /* debugging. 
     * TODO: document the first three methods
     */
    virtual void do_add(extmap::obj_offset unit, char *buf) = 0; 
    virtual void do_evict(int n) = 0;
    virtual void write_map(void) = 0;

    /* get superblock, flattened map, vector of free blocks, extent map
     * only returns ones where ptr!=NULL
     */
    virtual void get_info(j_read_super **p_super, extmap::obj_offset **p_flat, 
                          std::vector<int> **p_free_blks,
                          std::map<extmap::obj_offset,int> **p_map) = 0;
};

extern read_cache *make_read_cache(uint32_t blkno, int _fd, bool nt,
                                   translate *_be, objmap *_om, backend *_io);

#endif

