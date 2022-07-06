#ifndef TRANSLATE_H
#define TRANSLATE_H

#include "lsvd_includes.h"
#include "base_functions.h"

class translate {
    FILE *fp
	
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

    char *read_object_hdr(const char *name, bool fast);

    /* clone_info is variable-length, so we need to pass back pointers 
     * rather than values. That's OK because we allocate superblock permanently
     */
    typedef clone_info *clone_p;
    ssize_t read_super(const char *name, std::vector<uint32_t> &ckpts,
		       std::vector<clone_p> &clones, std::vector<snap_info> &snaps);

    ssize_t read_data_hdr(int seq, hdr &h, data_hdr &dh, std::vector<uint32_t> &ckpts,
		       std::vector<obj_cleaned> &cleaned, std::vector<data_map> &dmap);

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
    
    translate(backend *_io, objmap *omap) : workers(&m), misc_threads(&m) {
	io = _io;
	map = omap;
	current_batch = NULL;
	last_written = last_pushed = 0;
	fp = fopen("/tmp/xlate.log", "w");
    }
    ~translate() {
#if 1
	fprintf(fp, "xl: batches %ld (8MiB)\n", batches.size());
	fprintf(fp, "xl: in_mem %ld (%ld)\n", in_mem_objects.size(), sizeof(std::pair<int,char*>));
	fprintf(fp, "omap: %d %d (%ld)\n", map->map.size(), map->map.capacity(), sizeof(extmap::lba2obj));
	fprintf(fp, "gc %d read %d write %d delete %d\n", gc_cycles, gc_sectors_read, gc_sectors_written,
	    gc_deleted);
#endif
	while (!batches.empty()) {
	    auto b = batches.top();
	    batches.pop();
	    delete b;
	}
	if (current_batch)
	    delete current_batch;
	if (super)
	    free(super);
    }
    int checkpoint(void);
    int flush(void);
    ssize_t init(const char *name, int nthreads, bool timedflush);
    void shutdown(void);
    ssize_t readv(size_t offset, iovec *iov, int iovcnt);
    int inmem(int max, int *list);
    void getmap(int base, int limit, int (*cb)(void *ptr,int,int,int,int), void *ptr);
    int mapsize(void);
    void reset(void);
    int frontier(void);

}

#endif
