/*
 * file:        write_cache.h
 * description: full structure for write cache
 * 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef WRITE_CACHE_H
#define WRITE_CACHE_H

/* all addresses are in units of 4KB blocks
 */
class write_cache {
public:
    virtual void get_room(sector_t sectors) = 0; 
    virtual void release_room(sector_t sectors) = 0;

    virtual ~write_cache() {}

    virtual void writev(request *req, sector_t lba, smartiov *iov) = 0;
    virtual std::tuple<size_t,size_t,request*>
        async_read(size_t,char*,size_t) = 0;
    
    virtual void getmap(int base, int limit, int (*cb)(void*,int,int,int),
                        void *ptr) = 0;
    virtual void reset(void) = 0;
    virtual void get_super(j_write_super *s) = 0; /* copies superblock */
    virtual page_t get_oldest(page_t blk, std::vector<j_extent> &extents) = 0;
    virtual void do_write_checkpoint(void) = 0;
};

extern write_cache *make_write_cache(uint32_t blkno, int fd,
                                     translate *be, lsvd_config *cfg);

#endif


