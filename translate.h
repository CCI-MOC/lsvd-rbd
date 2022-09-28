// file:	translate.h
// description: this class focuses on the implementation of the translation layer to the system with
//              an object oriented approach. This file contains the following which things which are
//              documented below:
//                      -batch structure
//                      -translate class
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

#ifndef TRANSLATE_H
#define TRANSLATE_H

class batch;

class translate {
    std::mutex   m;
    objmap      *map;           /* map shared between read cache and translate */
    
    batch              *current_batch = NULL;
    std::stack<batch*>  batches;
    std::map<int,char*> in_mem_objects;

    /* info on all live objects - all sizes in sectors */
    struct obj_info {
	uint32_t hdr;
	uint32_t data;
	uint32_t live;
	int      type;
    };
    std::map<int,obj_info> object_info;

    char      *super_name;
    char      *super;
    hdr       *super_h;
    super_hdr *super_sh;
    size_t     super_len;

    std::atomic<int> puts_outstanding = 0;
    
    thread_pool<batch*> workers;
    thread_pool<int>    misc_threads;

    /* for flush()
     */
    int last_written = 0;
    int last_pushed = 0;
    std::condition_variable cv;

    /* for triggering GC
     */
    sector_t total_sectors = 0;
    sector_t total_live_sectors = 0;
    int gc_cycles = 0;
    int gc_sectors_read = 0;
    int gc_sectors_written = 0;
    int gc_deleted = 0;


    /* various methods to read:
     * - header from an object (returns pointer that must be freed)
     * - superblock object (returns size in bytes)
     * - header from a data object
     * - checkpoint object
     */
    char *read_object_hdr(const char *name, bool fast);

    typedef clone_info *clone_p;
    ssize_t read_super(const char *name, std::vector<uint32_t> &ckpts,
		       std::vector<clone_p> &clones, std::vector<snap_info> &snaps);

    ssize_t read_data_hdr(int seq, hdr &h, data_hdr &dh,
                          std::vector<uint32_t> &ckpts,
                          std::vector<obj_cleaned> &cleaned,
                          std::vector<data_map> &dmap);

    ssize_t read_checkpoint(int seq, std::vector<uint32_t> &ckpts,
                            std::vector<ckpt_obj> &objects, 
			    std::vector<deferred_delete> &deletes,
                            std::vector<ckpt_mapentry> &dmap);

    /* maybe use insertion sorted vector?
     *  https://stackoverflow.com/questions/15843525/how-do-you-insert-the-value-in-a-sorted-vector
     */

    /* completions may come in out of order; need to update the map in 
     * order of sequence numbers. 
     */
    std::set<std::pair<int32_t,void*>> completions;
    int32_t                            next_completion = 0;
    std::mutex                         m_c;
    std::condition_variable            cv_c;

    void do_completions(int32_t seq, void *closure);


    int write_checkpoint(int seq);

    int make_hdr(char *buf, batch *b);
    sector_t make_gc_hdr(char *buf, uint32_t seq, sector_t sectors,
			 data_map *extents, int n_extents);

    void do_gc(std::unique_lock<std::mutex> &lk);
    void gc_thread(thread_pool<int> *p);
    void worker_thread(thread_pool<batch*> *p);
    void ckpt_thread(thread_pool<int> *p);
    void flush_thread(thread_pool<int> *p);

public:
    backend *io;
    translate(backend *_io, objmap *omap);
    ~translate();

    ssize_t init(const char *name, int nthreads, bool timedflush);
    void shutdown(void);

    int flush(void);            /* write out current batch */
    int checkpoint(void);       /* flush, then write checkpoint */

    ssize_t writev(size_t offset, iovec *iov, int iovcnt);
    ssize_t readv(size_t offset, iovec *iov, int iovcnt);

    /* debug functions
     */
    void getmap(int base, int limit,
                int (*cb)(void *ptr,int,int,int,int), void *ptr);
    int mapsize(void);
    void reset(void);
    int frontier(void);

};

#endif
