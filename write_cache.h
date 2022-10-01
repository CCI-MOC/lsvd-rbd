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
    size_t         dev_max;
    uint32_t       super_blkno;

    std::atomic<int64_t> sequence; // write sequence #

    /* bookkeeping for write throttle
     */
    int total_write_pages = 0;
    int max_write_pages = 0;
    std::condition_variable write_cv;

    void evict(page_t base, page_t len);
    void send_writes(std::unique_lock<std::mutex> &lk);

    /* initialization stuff
     */
    void read_map_entries();
    void roll_log_forward();

    /* track length of each journal record in cache
     */
    std::map<page_t,int> lengths;

    thread_pool<int>          *misc_threads;

    void ckpt_thread(thread_pool<int> *p);
    bool ckpt_in_progress = false;
    void write_checkpoint(void);
    
    /* allocate journal entry, create a header
     */
    uint32_t allocate(page_t n, page_t &pad, page_t &n_pad);
    std::vector<request*> work;
    j_write_super *super;
    int            fd;          /* TODO: remove, use sync NVME */

public:

    /* throttle writes with window of max_write_pages
     */
    void get_room(int blks); 
    void release_room(int blks);

    /* these need to be public because they're used by
     * send_write_requests
     */
    std::mutex                m;
    extmap::cachemap2 map;
    extmap::cachemap2 rmap;
    bool              map_dirty;
    translate        *be;
    j_hdr *mk_header(char *buf, uint32_t type, uuid_t &uuid, page_t blks);
    
    nvme 		      *nvme_w = NULL;

    write_cache(uint32_t blkno, int _fd, translate *_be, int n_threads);
    ~write_cache();

    void writev(request *req);
    std::pair<size_t,size_t> async_read(size_t offset, char *buf, size_t bytes,
					void (*cb)(void*), void *ptr);

    /* debug functions */

    /* getmap callback(ptr, base, limit, phys_lba)
     */
    void getmap(int base, int limit, int (*cb)(void*,int,int,int), void *ptr);

    void reset(void);
    void get_super(j_write_super *s); /* copies superblock */

    /*  @blk - header to read
     *  @extents - data to move. empty if J_PAD or J_CKPT
     *  return value - first page of next record
     */
    page_t get_oldest(page_t blk, std::vector<j_extent> &extents);
    void do_write_checkpoint(void);
};

// aligned :	returns if ptr is properly aligned with a-1
bool aligned(const void *ptr, int a);

#endif


