#pragma once

#include <map>
#include <mutex>
#include <shared_mutex>

#include "backend.h"
#include "config.h"
#include "extent.h"
#include "lsvd_types.h"
#include "objects.h"
#include "shared_read_cache.h"
#include "utils.h"

class translate
{
  public:
    uuid_t uuid;
    uint64_t max_cache_seq;

    translate() {}
    virtual ~translate() {}

    virtual void shutdown(void) = 0;
    virtual void flush(void) = 0;      /* write out current batch */
    virtual void checkpoint(void) = 0; /* flush, then write checkpoint */

    virtual ssize_t writev(uint64_t cache_seq, size_t offset, iovec *iov,
                           int iovcnt) = 0;
    virtual ssize_t trim(size_t offset, size_t len) = 0;
    virtual void wait_for_room(void) = 0;

    virtual void object_read_start(int obj) = 0;
    virtual void object_read_end(int obj) = 0;

    virtual str prefix(seqnum_t seq) = 0; /* for read cache */
};

uptr<translate> make_translate(str name, lsvd_config &cfg, usize vol_size,
                               uuid_t &vol_uuid, sptr<backend> be,
                               sptr<read_cache> rcache, extmap::objmap &objmap,
                               std::shared_mutex &omap_mtx,
                               extmap::bufmap &bmap, std::mutex &bmap_lck,
                               seqnum_t last_seq, vec<clone_base> &clones,
                               std::map<seqnum_t, data_obj_info> &objinfo,
                               vec<seqnum_t> &checkpoints);

uptr<translate> make_translate(std::shared_ptr<backend> _io, lsvd_config *cfg,
                               extmap::objmap *map, extmap::bufmap *bufmap,
                               std::shared_mutex *m, std::mutex *buf_m,
                               sptr<read_cache> rcache);

int translate_create_image(sptr<backend> objstore, const char *name,
                           uint64_t size);
int translate_clone_image(sptr<backend> objstore, const char *source,
                          const char *dest);
int translate_remove_image(sptr<backend> objstore, const char *name);
int translate_get_uuid(sptr<backend> objstore, const char *name, uuid_t &uu);
