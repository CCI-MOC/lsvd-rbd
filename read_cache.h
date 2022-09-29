// file:	read_cache.h
// description: Full include file of the read_class for lsvd.
//		the read cache is:
//			 * 1. indexed by obj/offset[*], not LBA
//			 * 2. stores aligned 64KB blocks
//			 * [*] offset is in units of 64KB blocks
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

#ifndef READ_CACHE_H
#define READ_CACHE_H

#include "translate.h"

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

    void evict_thread(thread_pool<int> *p);
    
    char *get_cacheline_buf(int n); /* TODO: document this */

public:

    read_cache(uint32_t blkno, int _fd, bool nt,
               translate *_be, objmap *_om, backend *_io);
    ~read_cache();
    
    std::pair<size_t,size_t> async_read(size_t offset, char *buf, size_t len,
					void (*cb)(void*), void *ptr);

    /* debugging. 
     */

    /* get superblock, flattened map, vector of free blocks, extent map
     * only returns ones where ptr!=NULL
     */
    void get_info(j_read_super **p_super, extmap::obj_offset **p_flat, 
		  std::vector<int> **p_free_blks,
                  std::map<extmap::obj_offset,int> **p_map);

    void do_add(extmap::obj_offset unit, char *buf); /* TODO: document */
    void do_evict(int n);       /* TODO: document */
    void write_map(void);
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

