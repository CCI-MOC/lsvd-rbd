/*
write_cache.h : Full include file of the write_cache for lsvd.
*/

#ifndef WRITE_CACHE_H
#define WRITE_CACHE_H

/* all addresses are in units of 4KB blocks
 */
class write_cache {
    int            fd;
    size_t         dev_max;
    uint32_t       super_blkno;
    j_write_super *super;	// 4KB
    disk	  *dk;

    std::atomic<int64_t> sequence; // write sequence #
    
    extmap::cachemap2 map;
    extmap::cachemap2 rmap;
    std::map<page_t,int> lengths;
    translate        *be;
    bool              map_dirty;

    std::vector<cache_work*> work;
    int                      writes_outstanding = 0;

    page_t evict_trigger;
    
    thread_pool<int>          *misc_threads;
    std::mutex                m;
    std::condition_variable   alloc_cv;
    int                       nfree;
    
    char *pad_page;
    bool e_io_running = false;
    io_context_t ioctx;
    std::thread e_io_th;

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
// constructor for the write cache
    write_cache(uint32_t blkno, int _fd, translate *_be, int n_threads) {
	super_blkno = blkno;
	fd = _fd;
	dev_max = getsize64(fd);
	be = _be;
	char *buf = (char*)aligned_alloc(512, 4096);
	if (pread(fd, buf, 4096, 4096L*blkno) < 4096)
	    throw_fs_error("wcache");
	super = (j_write_super*)buf;
	pad_page = (char*)aligned_alloc(512, 4096);
	memset(pad_page, 0, 4096);
	map_dirty = false;
	
	if (super->map_entries) {
	    size_t map_bytes = super->map_entries * sizeof(j_map_extent),
		map_bytes_rounded = round_up(map_bytes, 4096);
	    char *map_buf = (char*)aligned_alloc(512, map_bytes_rounded);
	    std::vector<j_map_extent> extents;
	    if (pread(fd, map_buf, map_bytes_rounded, 4096L * super->map_start) < 0)
		throw_fs_error("wcache_map");
	    decode_offset_len<j_map_extent>(map_buf, 0, map_bytes, extents);
	    for (auto e : extents) {
		map.update(e.lba, e.lba+e.len, e.plba);
		rmap.update(e.plba, e.plba + e.len, e.lba);
	    }
	    free(map_buf);

	    size_t len_bytes = super->len_entries * sizeof(j_length),
		len_bytes_rounded = round_up(len_bytes, 4096);
	    char *len_buf = (char*)aligned_alloc(512, len_bytes_rounded);
	    std::vector<j_length> _lengths;
	    if (pread(fd, len_buf, len_bytes_rounded, 4096L * super->len_start) < 0)
		throw_fs_error("wcache_len");
	    decode_offset_len<j_length>(len_buf, 0, len_bytes, _lengths);
	    for (auto l : _lengths) {
		lengths[l.page] = l.len;
		assert(lengths[l.page] > 0);
		//xprintf("init: lens[%d] = %d\n", l.page, l.len);
	    }
	    free(len_buf);
	}

	/* TODO TODO TODO - need to roll log forward
	 */
	auto N = super->limit - super->base;
	if (super->oldest == super->next)
	    nfree = N - 1;
	else
	    nfree = (super->oldest + N - super->next) % N;

	int evict_min_pct = 5;
	int evict_max_mb = 100;
	evict_trigger = std::min((evict_min_pct * (int)(super->limit - super->base) / 100),
				 evict_max_mb * (1024*1024/4096));
	
	// https://stackoverflow.com/questions/22657770/using-c-11-multithreading-on-non-static-member-function

	misc_threads = new thread_pool<int>(&m);
	misc_threads->pool.push(std::thread(&write_cache::ckpt_thread, this, misc_threads));

	e_io_running = true;
	io_queue_init(64, &ioctx);
	const char *name = "write_cache_cb";
	e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
    }
// deconstructor for the write cache
    ~write_cache() {
#if 0
	printf("wc map: %d %d (%ld)\n", map.size(), map.capacity(), sizeof(extmap::lba2lba));
	printf("wc rmap: %d %d (%ld)\n", rmap.size(), rmap.capacity(), sizeof(extmap::lba2lba));
#endif
	free(pad_page);
	free(super);
	delete misc_threads;

	e_io_running = false;
	e_io_th.join();
	io_queue_release(ioctx);
    }

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


