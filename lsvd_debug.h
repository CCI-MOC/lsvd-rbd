/*
 * file:        lsvd_debug.h
 * description: debug function declarations
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef __LSVD_DEBUG_H__
#define __LSVD_DEBUG_H__

#include <queue>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <map>
#include <libaio.h>

#include "lsvd_types.h"
#include "extent.h"
#include "fake_rbd.h"
#include "smartiov.h"
#include "misc_cache.h"
#include "backend.h"
#include "translate.h"
#include "journal2.h"
#include "nvme.h"
#include "write_cache.h"
#include "read_cache.h"

/* TODO: do I need prototypes for all these functions?
 */

// dbg_lsvd_write :     do translation layer write
extern "C" int dbg_lsvd_write(rbd_image_t image, char *buffer,
                              uint64_t offset, uint32_t size);

// dbg_lsvd_read : 	do translation layer read
extern "C" int dbg_lsvd_read(rbd_image_t image, char *buffer,
                             uint64_t offset, uint32_t size);

// dbg_lsvd_flush :	Performs translation layer flush
extern "C" int dbg_lsvd_flush(rbd_image_t image);

// _dbg : debug structure, kind of the same as rbd_image_t
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

// xlate_open :	set up new file_backend, objmap, init translation layer
extern "C" int xlate_open(char *name, int n, bool flushthread, void **p);

// xlate_close : shuts down, deletes translate layer, objmap, backend
extern "C" void xlate_close(_dbg *d);

// xlate_flush :	flushes the translate layer of the debug structure
extern "C" int xlate_flush(_dbg *d);

// xlate_size :	returns mapsize of the translate layer of the debug structure
extern "C" int xlate_size(_dbg *d);

// xlate_read :	Performs translation layer read of the debug structure
extern "C" int xlate_read(_dbg *d, char *buffer, uint64_t offset,
                          uint32_t size);

// xlate_write :	Performs translation layer write of the debug structure
extern "C" int xlate_write(_dbg *d, char *buffer, uint64_t offset,
                           uint32_t size);

// tuple :	used for retrieving maps
struct tuple {
    int base;
    int limit;
    int obj;                    // object map
    int offset;
    int plba;                   // write cache map
};

// getmap_s :	more helper structures
struct getmap_s {
    int i;
    int max;
    struct tuple *t;
};

// xlate_getmap :	retrieve translation layer map
extern "C" int xlate_getmap(_dbg *d, int base, int limit, int max,
                            struct tuple *t);

// xlate_frontier :	current batch write frontier (in sectors???)
extern "C" int xlate_frontier(_dbg *d);

// xlate_reset :	resets translation layer
extern "C" void xlate_reset(_dbg *d);

// xlate_checkpoint :	calls checkpoint()
extern "C" int xlate_checkpoint(_dbg *d);

// wcache_open :	open write cache file / create cache instance
extern "C" void wcache_open(_dbg *d, uint32_t blkno, int fd, void **p);

// wcache_close :	close write cache file / delete instance
extern "C" void wcache_close(write_cache *wcache);

// wcache_read :	synchronous read from write cache
extern "C" void wcache_read(write_cache *wcache, char *buf,
                            uint64_t offset, uint64_t len);

// wcache_write :	synchronous write to write cache
extern "C" void wcache_write(write_cache *wcache, char *buf,
                             uint64_t offset, uint64_t len);

// wcache_img_write :	synchronous write, takes image_t instead of wcache
extern "C" void wcache_img_write(rbd_image_t image, char *buf,
                                 uint64_t offset, uint64_t len);

// wcache_getmap :	retrieve write cache map
extern "C" int wcache_getmap(write_cache *wcache, int base, int limit,
                             int max, struct tuple *t);

// wcache_get_super :	retrieve write cache superblock
extern "C" void wcache_get_super(write_cache *wcache, j_write_super *s);

// wcache_write_ckpt :	wcache->do_write_checkpoint()
extern "C" void wcache_write_ckpt(write_cache *wcache);

// wcache_oldest :	Returns info from the oldest journal entry in cache
extern "C" int wcache_oldest(write_cache *wcache, int blk, j_extent *extents,
                             int max, int *p_n);

// rcache_init :	open file, create new read cache
extern "C" void rcache_init(_dbg *d, uint32_t blkno, int fd, void **val_p);

// rcache_shutdown :	close file, delete read cache instance
extern "C" void rcache_shutdown(read_cache *rcache);

// rcache_evict :	calls do_evict on the read cache
extern "C" void rcache_evict(read_cache *rcache, int n);

// rcache_read :	read from read cache, skipping non-cached data
extern "C" void rcache_read(read_cache *rcache, char *buf,
                            uint64_t offset, uint64_t len);

// rcache_read2 :	damned if I remember what this one was for
extern "C" void rcache_read2(read_cache *rcache, char *buf,
                             uint64_t offset, uint64_t len);

// rcache_add :		add a 64KB block of data at indicated position
extern "C" void rcache_add(read_cache *rcache, int object,
                           int block, char *buf, size_t len);

// rcache_getsuper :	retrieve read cache superblock
extern "C" void rcache_getsuper(read_cache *rcache, j_read_super *p_super);

// rcache_getmap :	retrieve read cache map
extern "C" int rcache_getmap(read_cache *rcache, extmap::obj_offset *keys,
                             int *vals, int n);

// rcache_get_flat :	returns flattened read cache map 
extern "C" int rcache_get_flat(read_cache *rcache, extmap::obj_offset *vals,
                               int n);

// fakemap_update :	add fake entry to the translation layer map
extern "C" void fakemap_update(_dbg *d, int base, int limit,
                               int obj, int offset);

// fakemap_reset :	reset translation layer map
extern "C" void fakemap_reset(_dbg *d);

#endif
