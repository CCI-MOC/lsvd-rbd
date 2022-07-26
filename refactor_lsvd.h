#ifndef REFACTOR_LSVD_H
#define REFACTOR_LSVD_H

enum {
    EVENT_TYPE_PIPE = 1,
    EVENT_TYPE_EVENTFD = 2
};
    
typedef void *rbd_image_t;
typedef void *rbd_image_options_t;
typedef void *rbd_pool_stats_t;

typedef void *rbd_completion_t;
typedef void (*rbd_callback_t)(rbd_completion_t cb, void *arg);

#define RBD_MAX_BLOCK_NAME_SIZE 24
#define RBD_MAX_IMAGE_NAME_SIZE 96

/* fio only looks at 'size' */
typedef struct {
  uint64_t size;
  uint64_t obj_size;
  uint64_t num_objs;
  int order;
  char block_name_prefix[RBD_MAX_BLOCK_NAME_SIZE]; /* deprecated */
  int64_t parent_pool;                             /* deprecated */
  char parent_name[RBD_MAX_IMAGE_NAME_SIZE];       /* deprecated */
} rbd_image_info_t;

typedef struct {
  uint64_t id;
  uint64_t size;
  const char *name;
} rbd_snap_info_t;

/* now our fake implementation
 */
struct fake_rbd_image {
    std::mutex   m;
    backend     *io;
    objmap      *omap;
    translate   *lsvd;
    write_cache *wcache;
    read_cache  *rcache;
    ssize_t      size;          // bytes
    int          fd;            // cache device
    j_super     *js;            // cache page 0
    bool         notify;
    int          eventfd;
    std::queue<rbd_completion_t> completions;
};

struct lsvd_completion {
public:
    fake_rbd_image *fri;
    rbd_callback_t cb;
    void *arg;
    int retval;
    bool done = false;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<int> refcount = 0;
    std::atomic<int> n = 0;
    iovec iov;                  // occasional use only
    
    lsvd_completion() {}
// get :	increments the refcount
    void get(void);
// put :	decrements the refcount and deletes the lsvd_completion object if refcount == 0 after decrement
    void put(void);
// complete :	writes eventfd, pushes completion
    void complete(int val);
};

// rbd_poll_io_events :	Reads in rbd image and reads in completions to the location of the inputted
//			rbd_completion_t object until either no more completions are left in image
//			or the max limit, numcomp is reached. Pops each completion in image after reading it.
extern "C" int rbd_poll_io_events(rbd_image_t image, rbd_completion_t *comps, int numcomp);

// rbd_set_image_notification :	If type inputted is EVENT_TYPE_EVENTFD, sets image notify to true and eventfd to fd
extern "C" int rbd_set_image_notification(rbd_image_t image, int fd, int type);

// rbd_aio_create_completion :	Initializes new lsvd_completion with callback of complete_cb and argument
//				of cb_arg and sets it to pointer c.
extern "C" int rbd_aio_create_completion(void *cb_arg, rbd_callback_t complete_cb, rbd_completion_t *c);

// rbd_aio_release :	Converts rbd_completion_t to lsvd_completion and utilizes lsvd put function seen above.
//			Releases reference to the aio
extern "C" void rbd_aio_release(rbd_completion_t c);

// rbd_aio_discard :	Converts rbd_completion_t to lsvd_completion and sets completion image to inputted image
//			and calls complete(0)
extern "C" int rbd_aio_discard(rbd_image_t image, uint64_t off, uint64_t len, rbd_completion_t c);

//????????
extern "C" int rbd_aio_flush(rbd_image_t image, rbd_completion_t c);

// rbd_flush :	Converts rbd_image_t to lsvd fake_rbd_image and then flushes the translation layer and writes
//		a checkpoint
extern "C" int rbd_flush(rbd_image_t image);

// rbd_aio_get_arg :	converts rbd_completion_t to lsvd_completion and returns arg of lsvd_completion
extern "C" void *rbd_aio_get_arg(rbd_completion_t c);

// rbd_aio_get_return_value :	converts rbd_completion_t to lsvd_completion and returns retval of lsvd_completion
extern "C" ssize_t rbd_aio_get_return_value(rbd_completion_t c);

// rbd_aio_read :	aligns buffer to 512 byte boundary if it isn't, then performs async read on rbd_image_t
//			with a given offset, len, into the buf
extern "C" int rbd_aio_read(rbd_image_t image, uint64_t offset, size_t len, char *buf, rbd_completion_t c);

// rbd_aio_readv :	Empty function definition
extern "C" int rbd_aio_readv(rbd_image_t image, const iovec *iov, int iovcnt, uint64_t off, rbd_completion_t c);

// rbd_aio_writev :      Empty function definition
extern "C" int rbd_aio_writev(rbd_image_t image, const struct iovec *iov, int iovcnt, uint64_t off, rbd_completion_t c);

// rbd_aio_write :	aligns buffer to 512 byte boundary, wraps lsvd_completion, and then calls wcache write
extern "C" int rbd_aio_write(rbd_image_t image, uint64_t off, size_t len, const char *buf, rbd_completion_t c);

// rbd_call_wrapped :	Wraps ptr, rbd_completion included to fit function format but isn't used
void rbd_call_wrapped(rbd_completion_t c, void *ptr);

// rbd_read :	calls rbd_aio_create_completion and then rbd_aio_read using image, off, len, and buf inputted
//		returns rbd_aio_get_return_value.
extern "C" int rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf);

// rbd_write :	calls rbd_aio_create_completion and then rbd_aio_write using image, off, len, and buf inputted
//              returns rbd_aio_get_return_value.
extern "C" int rbd_write(rbd_image_t image, uint64_t off, size_t len, const char *buf);

// rbd_aio_wait_for_complete :	increments ref_count until lock is released and decrements ref_count back
extern "C" int rbd_aio_wait_for_complete(rbd_completion_t c);

// rbd_stat :	set rbd_image_info_t size to the size in the rbd_image, infosize is not used.
extern "C" int rbd_stat(rbd_image_t image, rbd_image_info_t *info, size_t infosize);

// rbd_get_size :	set size to point to the size of rbd image
extern "C" int rbd_get_size(rbd_image_t image, uint64_t *size);

fake_rbd_image *the_fri;        // debug

// rbd_open :	Creates a new image, file and rados backend, read and write cache, and initializes translate layer
//		Opens file based on given name and essentially sets everything necessary for interacting 
//		with rbd image
extern "C" int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image, const char *snap_name);

// rbd_close :	takes rbd_image, writes back the read cache map to backend, writes write cache checkpoint,
//		flushes and writes back a checkpoint for translation layer, then deletes each part of the image
extern "C" int rbd_close(rbd_image_t image);

// rbd_invalidate_cache :	function stub
extern "C" int rbd_invalidate_cache(rbd_image_t image);
/* These RBD functions are unimplemented and return errors
 */
extern "C" int rbd_create(rados_ioctx_t io, const char *name, uint64_t size, int *order);
extern "C" int rbd_resize(rbd_image_t image, uint64_t size);
extern "C" int rbd_snap_create(rbd_image_t image, const char *snapname);
extern "C" int rbd_snap_list(rbd_image_t image, rbd_snap_info_t *snaps, int *max_snaps);
extern "C" void rbd_snap_list_end(rbd_snap_info_t *snaps);
extern "C" int rbd_snap_remove(rbd_image_t image, const char *snapname);
extern "C" int rbd_snap_rollback(rbd_image_t image, const char *snapname);

/* debug functions
 */
/*
// dbg_lsvd_write :	do translation layer write
extern "C" int dbg_lsvd_write(rbd_image_t image, char *buffer, uint64_t offset, uint32_t size);
// dbg_lsvd_read : 	do translation layer read
extern "C" int dbg_lsvd_read(rbd_image_t image, char *buffer, uint64_t offset, uint32_t size);
// dbg_lsvd_flush :	Performs translation layer flush
extern "C" int dbg_lsvd_flush(rbd_image_t image);

// _dbg :	structure used specifically for debugging, creating separate translate layer and caches
//		to perform functions on
struct _dbg {
public:
    int type = 0;
    translate   *lsvd;
    write_cache *wcache;
    objmap      *omap;
    read_cache  *rcache;
    backend     *io;
    _dbg(int _t, translate *_l, write_cache *_w, objmap *_o, read_cache *_r, backend *_io) :
        type(_t), lsvd(_l), wcache(_w), omap(_o), rcache(_r), io(_io) {}
};

// xlate_open :	sets up new file_backend, objmap, initializes translate layer
extern "C" int xlate_open(char *name, int n, bool flushthread, void **p);
extern "C" void xlate_close(_dbg *d);
extern "C" int xlate_flush(_dbg *d);
extern "C" int xlate_size(_dbg *d);
extern "C" int xlate_read(_dbg *d, char *buffer, uint64_t offset, uint32_t size);
extern "C" int xlate_write(_dbg *d, char *buffer, uint64_t offset, uint32_t size);

struct tuple {
    int base;
    int limit;
    int obj;                    // object map
    int offset;
    int plba;                   // write cache map
};

struct getmap_s {
    int i;
    int max;
    struct tuple *t;
};

int getmap_cb(void *ptr, int base, int limit, int obj, int offset);

extern "C" int xlate_getmap(_dbg *d, int base, int limit, int max, struct tuple *t);
extern "C" int xlate_frontier(_dbg *d);
extern "C" void xlate_reset(_dbg *d);
extern "C" int xlate_checkpoint(_dbg *d);

extern "C" void wcache_open(_dbg *d, uint32_t blkno, int fd, void **p);
extern "C" void wcache_close(write_cache *wcache);
extern "C" void wcache_read(write_cache *wcache, char *buf, uint64_t offset, uint64_t len);
extern "C" void wcache_write(write_cache *wcache, char *buf, uint64_t offset, uint64_t len);
extern "C" void wcache_img_write(rbd_image_t image, char *buf, uint64_t offset, uint64_t len);

extern "C" void wcache_reset(write_cache *wcache);
int wc_getmap_cb(void *ptr, int base, int limit, int plba);
extern "C" int wcache_getmap(write_cache *wcache, int base, int limit, int max, struct tuple *t);
extern "C" void wcache_get_super(write_cache *wcache, j_write_super *s);
extern "C" void wcache_write_ckpt(write_cache *wcache);
extern "C" int wcache_oldest(write_cache *wcache, int blk, j_extent *extents, int max, int *p_n);

extern "C" void rcache_init(_dbg *d, uint32_t blkno, int fd, void **val_p);
extern "C" void rcache_shutdown(read_cache *rcache);
extern "C" void rcache_evict(read_cache *rcache, int n);
extern "C" void rcache_read(read_cache *rcache, char *buf, uint64_t offset, uint64_t len);
extern "C" void rcache_read2(read_cache *rcache, char *buf, uint64_t offset, uint64_t len);
extern "C" void rcache_add(read_cache *rcache, int object, int block, char *buf, size_t len);
extern "C" void rcache_getsuper(read_cache *rcache, j_read_super *p_super);
extern "C" int rcache_getmap(read_cache *rcache, extmap::obj_offset *keys, int *vals, int n);
extern "C" int rcache_get_flat(read_cache *rcache, extmap::obj_offset *vals, int n);
extern "C" void rcache_reset(read_cache *rcache);

extern "C" void fakemap_update(_dbg *d, int base, int limit, int obj, int offset);
extern "C" void fakemap_reset(_dbg *d);
*/
#endif
