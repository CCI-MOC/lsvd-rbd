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
    //j_write_super *super;	// 4KB

    std::atomic<int64_t> sequence; // write sequence #
    
    extmap::cachemap2 map;
    extmap::cachemap2 rmap;
    std::map<page_t,int> lengths;
    translate        *be;
    bool              map_dirty;

    std::vector<cache_work*> work;
    int                      writes_outstanding = 0;
    int fp;
    page_t evict_trigger;
    
    thread_pool<int>          *misc_threads;
    std::mutex                m;
    std::condition_variable   alloc_cv;
    int                       nfree;
    
    char *pad_page;
  //bool e_io_running = false;
  //std::thread e_io_th;
    
    page_t blocks;
    char*  pad_hdr;
    nvme 		      *nvme_w;
    nvme_request 	      *parent_request;
// pages_free :	returns the number of free pages left inside of the write_cache super block
    int pages_free(uint32_t oldest);

// allocate_locked : 	allocates a number of pages n with a mutex lock. If not enough pages are available
// 			the current super->next pointer is set to &pad, and pages starting with the base
//			of the write cache super block are allocated. Returns the pointer to the beginning of
//			allocated data.
    uint32_t allocate_locked(page_t n, page_t &pad,
                             std::unique_lock<std::mutex> &lk);

// mk_header :	sets up a j_hdr structure which functions as a header for the write_cache with the preset of 
//		the type of header and the number of blocks, returning a pointer 
//		to that structure, and the set up UUID for the structure inside &uuid
    j_hdr *mk_header(char *buf, uint32_t type, uuid_t &uuid, page_t blks);
  void append_seq(void);
// evict :	Checks which pages are free and based on the oldest and deletes them from the cache
    void evict(void);

// ckpt_thread :	Writes a checkpoint if new data has been written to a thread but not recorded in a 
//			checkpoint within a particular timespan
    void ckpt_thread(thread_pool<int> *p);
    bool ckpt_in_progress = false;

// write_checkpoint : 	writes the super block information and io information back to file, creating 
//			a save of current write cache metadata and IOPS
    void write_checkpoint(void);

public:
    j_write_super *super;
    int            fd;
  //io_context_t ioctx;
    page_t blockno;
// constructor for the write cache
    write_cache(uint32_t blkno, int _fd, translate *_be, int n_threads);
// deconstructor for the write cache
    ~write_cache();

// send_writes :	clears write_cache work, writes back IOPS, then updates maps, writes the cache
//			to backend and then clears up all temporary variables
    void send_writes(void);
    bool evicting = false;

// writev :	calls to evict if pages_free > evict trigger, then cachework added to work, and then sends
//		writes to backend
    void writev(size_t offset, const iovec *iov, int iovcnt, void (*cb)(void*), void *ptr);

// async_read :	Performs an asynchronous read similar to the read in read_cache without additional
//		functionality.
    std::pair<size_t,size_t> async_read(size_t offset, char *buf, size_t bytes,
					void (*cb)(void*), void *ptr);

// getmap :	retrieves information about the map and stores it in separate variables
    void getmap(int base, int limit, int (*cb)(void*, int, int, int), void *ptr);

// reset :	resets the map
    void reset(void);

// get_super :	copies the pointer to the write_cache superblock to s
    void get_super(j_write_super *s);
    /* debug:
     * cache eviction - get info from oldest entry in cache. [should be private]
     *  @blk - header to read
     *  @extents - data to move. empty if J_PAD or J_CKPT
     *  return value - first page of next record
     */
    page_t get_oldest(page_t blk, std::vector<j_extent> &extents);

// do_write_checkpoint : if map is dirty, writes a checkpoint
    void do_write_checkpoint(void);
};

// aligned :	returns if ptr is properly aligned with a-1
bool aligned(const void *ptr, int a);

#endif


