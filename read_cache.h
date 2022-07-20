/* 
read_cache.h : Full include file of the read_class for lsvd. 
the read cache is:
 * 1. indexed by obj/offset[*], not LBA
 * 2. stores aligned 64KB blocks 
 * [*] offset is in units of 64KB blocks


*/

#ifndef READ_CACHE_H
#define READ_CACHE_H

class read_cache {
    
    std::mutex m;
    std::map<extmap::obj_offset,int> map;

    j_read_super       *super;
    extmap::obj_offset *flat_map;
    objmap             *omap;
    translate          *be;
    int                 fd;
    size_t              dev_max;
    backend            *io;
    
    int               unit_sectors;
    std::vector<int>  free_blks;
    bool              map_dirty = false;


    // new idea for hit rate - require that sum(backend reads) is no
    // more than 2 * sum(read sectors) (or 3x?), using 64bit counters 
    //
    struct {
	int64_t       user = 1000; // hack to get test4/test_2_fakemap to work
	int64_t       backend = 0;
    } hit_stats;
    
    thread_pool<int> misc_threads; // eviction thread, for now
    bool             nothreads = false;	// for debug

    /* if map[obj,offset] = n:
     *   in_use[n] - not eligible for eviction
     *   written[n] - safe to read from cache
     *   buffer[n] - in-memory data for block n
     *   pending[n] - continuations to invoke when buffer[n] becomes valid
     * buf_loc - FIFO queue of {n | buffer[n] != NULL}
     */
    sized_vector<std::atomic<int>>   in_use;
    sized_vector<char>               written; // can't use vector<bool> here
    sized_vector<char*>              buffer;
    sized_vector<std::vector<void*>> pending;
    std::queue<int>    buf_loc;
    
    io_context_t ioctx;
    std::thread e_io_th;
    bool e_io_running = false;
    
    /* possible CLOCK implementation - queue holds <block,ojb/offset> 
     * pairs so that we can evict blocks without having to remove them 
     * from the CLOCK queue
     */
    sized_vector<char> a_bit;
#if 0
    sized_vector<int>  block_version;
    std::queue<std::pair<int,extmap::obj_offset>> clock_queue;
#endif
    
    /* evict 'n' blocks - random replacement
     */
// evict :	Frees n number of blocks and erases oo from the map
    void evict(int n);

// evict_thread :	Frees used units/4 - size of free blocks and evicts n. Writes map after evicting
//			or when the map is dirty
    void evict_thread(thread_pool<int> *p);
    
public:
    std::atomic<int> n_lines_read = 0;

// read_cache : Constructor for the read cache
    read_cache(uint32_t blkno, int _fd, bool nt, translate *_be, objmap *_om, backend *_io) :
        omap(_om), be(_be), fd(_fd), io(_io), misc_threads(&m), nothreads(nt)
    {
        dev_max = getsize64(fd);
        n_lines_read = 0;
        char *buf = (char*)aligned_alloc(512, 4096);
        if (pread(fd, buf, 4096, 4096L*blkno) < 4096)
            throw_fs_error("rcache3");
        super = (j_read_super*)buf;

        assert(super->unit_size == 128); // 64KB, in sectors
        unit_sectors = super->unit_size; // todo: fixme

        int oos_per_pg = 4096 / sizeof(extmap::obj_offset);
        assert(div_round_up(super->units, oos_per_pg) == super->map_blocks);

        flat_map = (extmap::obj_offset*)aligned_alloc(512, super->map_blocks*4096);
        if (pread(fd, (char*)flat_map, super->map_blocks*4096, super->map_start*4096L) < 0)
            throw_fs_error("rcache2");

        for (int i = 0; i < super->units; i++) {
            if (flat_map[i].obj != 0) 
                map[flat_map[i]] = i;
            else 
                free_blks.push_back(i);
        }

        in_use.init(super->units);
        written.init(super->units);
        buffer.init(super->units);
        pending.init(super->units);
        a_bit.init(super->units);

        map_dirty = false;

        misc_threads.pool.push(std::thread(&read_cache::evict_thread, this, &misc_threads));

        io_queue_init(64, &ioctx);
        e_io_running = true;
        const char *name = "read_cache_cb";
        e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
    }


// get_cacheline_buf :	returns the pointer to the buffer[j] where j = buf_loc.front(),
//			and then pop and erase buffer[j]
    char *get_cacheline_buf(int n);
    
    int u1 = 0;
    int u0 = 0;

// async_read :
    std::pair<size_t,size_t> async_read(size_t offset, char *buf, size_t len,
					void (*cb)(void*), void *ptr);

// write_map : 	writes map back to file
    void write_map(void);

    ~read_cache() {
#if 1
	printf("rc: map %ld (%ld)\n", map.size(), sizeof(std::pair<extmap::obj_offset,int>));
	printf("rc: usecache 1 %d 0 %d (stat.u %ld .b %ld)\n", u1, u0, hit_stats.user, hit_stats.backend);
#endif
	free((void*)flat_map);
	free((void*)super);

	e_io_running = false;
	e_io_th.join();
	io_queue_release(ioctx);
    }

    /* debugging. 
     */
// get_info :	sets p_super, p_flat, p_free_blks, p_map to point at super, flat_map, &free_blks, &map, respectively
//		if those are not NULL
    void get_info(j_read_super **p_super, extmap::obj_offset **p_flat, 
		  std::vector<int> **p_free_blks, std::map<extmap::obj_offset,int> **p_map);

// do_add :	adds unit to the free_block back index of flat map and writes map
    void do_add(extmap::obj_offset unit, char *buf); 

// do_evict :	calls the evict function to evict n number of blocks with a lock
    void do_evict(int n);

// reset : 	Empty function definition
    void reset(void);


};

    /* state machine for block obj,offset can be represented by the tuple:
     *  map=n - i.e. exists(n) | map[obj,offset] = n
     *  in_use[n] - 0 / >0
     *  written[n] - n/a, F, T
     *  buffer[n] - n/a, NULL, <p>
     *  pending[n] - n/a, [], [...]
     *
     * if not cached                          -> {!map, n/a}
     * first read will:
     *   - add to map
     *   - increment in_use
     *   - launch read                        -> {map=n, >0, F, NULL, []}
     * following reads will 
     *   queue lambdas to copy from buffer[n] -> {map=n, >0, F, NULL, [..]}
     * read complete will:
     *   - set buffer[n]
     *   - invoke lambdas from pending[*]
     *   - launch write                       -> {map=n, >0, F, <p>, []}
     * write complete will:
     *   - set 'written' to true              -> {map=n, >0, T, <p>, []}
     * eviction of buffer will:
     *   - decr in_use
     *   - remove buffer                      -> {map=n, 0, NULL, []}
     * further reads will temporarily increment in_use
     * eviction will remove from map:         -> {!map, n/a}
     */


#endif

