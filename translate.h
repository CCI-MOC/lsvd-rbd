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

// batch:	structure which is used specifically by the translate class, keeping track of
//		a datamap of a specific part of information currently in the translation layer
//		with related information to this datamap.
//		Has its own unique constructure which defines the buffer and max
//		and deconstructer which frees the buf. Finally has a few functions which perform
//		modifications on the batch (see below)
struct batch {
    char  *buf;
    size_t max;
    size_t len;
    int    seq;
    std::vector<data_map> entries;
public:
// batch :	constructor for batch class
    batch(size_t _max) {
        buf = (char*)malloc(_max);
        max = _max;
    }

// ~batch :	deconstructor for batch class
    ~batch() {
        free((void*)buf);
    }

// reset :	resets the length and vector of entries of the batch to zero
    void reset(void);

// append_iov :	Adds additional iov entries onto the end of the batch entries
    void append_iov(uint64_t lba, iovec *iov, int iovcnt);

// hdrlen :	Returns the size of hdr + data_hdr + size of all batch entries
    int hdrlen(void);
};

class translate {
    FILE *fp;
	
    std::mutex   m;
    objmap      *map;
    
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

// read_object_hdr :	returns the hdr object copied from io based on name using read_object function
    char *read_object_hdr(const char *name, bool fast);

    /* clone_info is variable-length, so we need to pass back pointers 
     * rather than values. That's OK because we allocate superblock permanently
     */
    typedef clone_info *clone_p;

// read_super :	returns size of super_h + 1 based on read hdr and appends clones with the current superblock
    ssize_t read_super(const char *name, std::vector<uint32_t> &ckpts,
		       std::vector<clone_p> &clones, std::vector<snap_info> &snaps);

/* need more documentation*/
// read_data_hdr :	sets h to point to hdr and dh to data_hdr, then decodes ckpts, cleaned, dmap
    ssize_t read_data_hdr(int seq, hdr &h, data_hdr &dh, std::vector<uint32_t> &ckpts,
		       std::vector<obj_cleaned> &cleaned, std::vector<data_map> &dmap);

// read_checkpoint :	decodes ckpts, objects, deletes, dmap
    ssize_t read_checkpoint(int seq, std::vector<uint32_t> &ckpts, std::vector<ckpt_obj> &objects, 
			    std::vector<deferred_delete> &deletes, std::vector<ckpt_mapentry> &dmap);

    /* completions may come in out of order; need to re-order them before 
     * updating the map. This is the only modifier of 'completions', 
     * 'next_completion' - use a separate mutex for simplicity / deadlock avoidance
     * maybe use insertion sorted vector?
     *  https://stackoverflow.com/questions/15843525/how-do-you-insert-the-value-in-a-sorted-vector
     */
    std::set<std::pair<int32_t,void*>> completions;
    int32_t                            next_completion = 0;
    std::mutex                         m_c;
    std::condition_variable            cv_c;

// do_completions :	Wraps closure if seq is next completion, otherwise begins completion, wraps the 
//			rest of the completions
    void do_completions(int32_t seq, void *closure);

// write_checkpoint :	Writes checkpoint for the translate layer
    int write_checkpoint(int seq);

// make_hdr :	typecasts buf to hdr and populates the structure with hdr initialization
//		creates a data_hdr structure nad then populates it as well, sets a pointer to data map
//		and increments the pointer based on number of entries in batch and returns a pointer
// 		to the start of the hdr
    int make_hdr(char *buf, batch *b);

// make_gc_hdr :	Performs the same operations as make_hdr, except instead of using batches
//			to define pointers, uses direct information about sequence, sectors, and extents.
//			Returns the number of hdr_sectors
    sector_t make_gc_hdr(char *buf, uint32_t seq, sector_t sectors,
			 data_map *extents, int n_extents);

// do_gc :	
    void do_gc(std::unique_lock<std::mutex> &lk);

// gc_thread :	Calls do_gc when the total_live_sectors goes over a particular amount for the thread_pool
//		while the thread_pool is running with a unique lock
    void gc_thread(thread_pool<int> *p);

// worker_thread :	
    void worker_thread(thread_pool<batch*> *p);

// ckpt_thread :	sets a checkpoint for the inputted thread_pool
    void ckpt_thread(thread_pool<int> *p);

// flush_thread :	Tests if thread_pool is running, waits, and then flushes when threads reach timeout
    void flush_thread(thread_pool<int> *p);

public:
//    disk    *dk;
    backend *io;
// translate :	constructor for translate class
    translate(backend *_io, objmap *omap);

// ~translate :	deconstructor for translate class
    ~translate();

// checkpoint:	Takes current batch, calls put_locked on it if it is active and length is not zero, resets it
//		and writes it as a checkpoint
    int checkpoint(void);
// flush:	Just calls put_locked on current_batch, waits until last_written = last_pushed and then returns
//		current_batch->seq
    int flush(void);
// init :	Initializes variables for translate layer
    ssize_t init(const char *name, int nthreads, bool timedflush);
// shutdown : 	empty function definition ***
    void shutdown(void);
// writev : 	Writes at the translation layer using the data in the current batch, and then empties the batch
    ssize_t writev(size_t offset, iovec *iov, int iovcnt);
// readv :	Simply reads at the translation layer
    ssize_t readv(size_t offset, iovec *iov, int iovcnt);
// getmap :	stores map information in ptr
    void getmap(int base, int limit, int (*cb)(void *ptr,int,int,int,int), void *ptr);
// mapsize :	returns translate objmap size
    int mapsize(void);
// reset :	resets the objmap of the translate class
    void reset(void);
// frontier :	returns the length of current batch if current batch exists. Otherwise returns 0
    int frontier(void);

};

#endif
