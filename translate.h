#ifndef TRANSLATE_H
#define TRANSLATE_H

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
// translate :	constructor for translate class
    translate(backend *_io, objmap *omap) : workers(&m), misc_threads(&m) {
	io = _io;
	map = omap;
	current_batch = NULL;
	last_written = last_pushed = 0;
	fp = fopen("/tmp/xlate.log", "w");
    }

// ~translate :	deconstructor for translate class
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
// init :	Initializes variables for translate layer including 
    ssize_t init(const char *name, int nthreads, bool timedflush);
// shutdown : empty function definition ***
    void shutdown(void);
    ssize_t writev(size_t offset, iovec *iov, int iovcnt);
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
