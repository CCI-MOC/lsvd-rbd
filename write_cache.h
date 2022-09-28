// file:	write_cache.h
// description: Full include file of the write_cache for LSVD
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

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

    /* initialization stuff
     */
    void read_map_entries();
    void roll_log_forward();

public:

    /* throttle writes with window of max_write_pages
     */
    void get_room(int blks); 
    void release_room(int blks);
    
    extmap::cachemap2 map;
    extmap::cachemap2 rmap;

    /* track the length of each journal record in cache
     */
    std::map<page_t,int> lengths;

    translate        *be;

    bool              map_dirty;

    std::vector<cache_work*> work;

    thread_pool<int>          *misc_threads;
    std::mutex                m;
    int                       nfree;
    
    char *pad_page;

    nvme_request 	      *parent_request;
    int pages_free(uint32_t oldest);

    /* allocate journal entry, create a header
     */
    uint32_t allocate(page_t n, page_t &pad, page_t &n_pad);
    j_hdr *mk_header(char *buf, uint32_t type, uuid_t &uuid, page_t blks);


// ckpt_thread :	Writes a checkpoint if new data has been written to a thread but not recorded in a 
//			checkpoint within a particular timespan
    void ckpt_thread(thread_pool<int> *p);
    bool ckpt_in_progress = false;

// write_checkpoint : 	writes the super block information and io information back to file, creating 
//			a save of current write cache metadata and IOPS
    void write_checkpoint(void);
    
    nvme 		      *nvme_w;

    j_write_super *super;
    int            fd;
    
// constructor for the write cache
    write_cache(uint32_t blkno, int _fd, translate *_be, int n_threads);
// deconstructor for the write cache
    ~write_cache();

// send_writes :	clears write_cache work, writes back IOPS, then updates maps, writes the cache
//			to backend and then clears up all temporary variables
    void send_writes(std::unique_lock<std::mutex> &lk);
    bool evicting = false;

    void writev(size_t offset, const iovec *iov, int iovcnt,
                void (*cb)(void*), void *ptr);

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


