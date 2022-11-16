/*
 * file:        lsvd_debug.h
 * description: extern functions for unit tests
 */

#ifndef __LSVD_DEBUG_H__
#define __LSVD_DEBUG_H__

class translate;
class write_cache;
class objmap;
class read_cache;
class backend;

/* types used to interface with some debug functions - must
 * match ctypes definitions in lsvd_types.py
 */
struct _dbg {
public:
    int type = 0;
    translate   *lsvd;
    write_cache *wcache;
    objmap      *omap;
    read_cache  *rcache;
    backend     *io;
    _dbg(int _t, translate *_l, write_cache *_w, objmap *_o,
         read_cache *_r, backend *_io) : type(_t), lsvd(_l), wcache(_w),
        omap(_o), rcache(_r), io(_io) {}
};

// tuple :      used for retrieving maps
struct tuple {
    int base;
    int limit;
    int obj;                    // object map
    int offset;
    int plba;                   // write cache map
};

// getmap_s :   more helper structures
struct getmap_s {
    int i;
    int max;
    struct tuple *t;
};

// copied from extent.h
struct obj_offset {
    int64_t obj    : 36;
    int64_t offset : 28;    // 128GB
};

struct j_extent;
struct j_write_super;
struct j_read_super;

extern "C" int dbg_lsvd_write(rbd_image_t image, char *buffer, uint64_t offset, uint32_t size);
extern "C" int dbg_lsvd_read(rbd_image_t image, char *buffer, uint64_t offset, uint32_t size);;
extern "C" int dbg_lsvd_flush(rbd_image_t image);
extern "C" int xlate_open(char *name, int n, bool flushthread, void **p);
extern "C" void xlate_close(_dbg *d);
extern "C" int xlate_flush(_dbg *d);
extern "C" int xlate_size(_dbg *d);
extern "C" int xlate_read(_dbg *d, char *buffer, uint64_t offset, uint32_t size);
extern "C" int xlate_write(_dbg *d, char *buffer, uint64_t offset, uint32_t size);
extern "C" int xlate_getmap(_dbg *d, int base, int limit, int max, struct tuple *t);
extern "C" int xlate_frontier(_dbg *d);
extern "C" int xlate_seq(_dbg *d);
extern "C" void xlate_reset(_dbg *d);
extern "C" int xlate_checkpoint(_dbg *d);
extern "C" void wcache_open(_dbg *d, uint32_t blkno, int fd, void **p);
extern "C" void wcache_close(write_cache *wcache);
extern "C" void wcache_read(write_cache *wcache, char *buf, uint64_t offset, uint64_t len);
extern "C" void wcache_img_write(rbd_image_t image, char *buf, uint64_t offset, uint64_t len);
extern "C" void wcache_write(write_cache *wcache, char *buf, uint64_t offset, uint64_t len);
extern "C" int wcache_getmap(write_cache *wcache, int base, int limit, int max, struct tuple *t);
extern "C" void wcache_get_super(write_cache *wcache, j_write_super *s);
extern "C" void wcache_write_ckpt(write_cache *wcache);
extern "C" int wcache_oldest(write_cache *wcache, int blk, j_extent *extents, int max, int *p_n);
extern "C" void rcache_init(_dbg *d, uint32_t blkno, int fd, void **val_p);
extern "C" void rcache_shutdown(read_cache *rcache);
extern "C" void rcache_evict(read_cache *rcache, int n);
extern "C" void rcache_read(read_cache *rcache, char *buf,
                            uint64_t offset, uint64_t len);
extern "C" void rcache_read2(read_cache *rcache, char *buf,
                             uint64_t offset, uint64_t len);
extern "C" void rcache_add(read_cache *rcache, int object, int block, char *buf, size_t len);
extern "C" void rcache_getsuper(read_cache *rcache, j_read_super *p_super);
extern "C" int rcache_getmap(read_cache *rcache,
                             obj_offset *keys, int *vals, int n);
extern "C" int rcache_get_flat(read_cache *rcache, obj_offset *vals, int n);
extern "C" void fakemap_update(_dbg *d, int base, int limit,
                               int obj, int offset);
extern "C" void fakemap_reset(_dbg *d);

extern "C" void get_rbd_uuid(rbd_image_t image, uuid_t *uuid);

/* lightweight printf to buffer, retrieve via get_logbuf or lsvd.logbuf
 */
extern void do_log(const char *fmt, ...);
extern "C" int get_logbuf(char *buf);

#endif
