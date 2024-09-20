#include <boost/uuid/uuid.hpp>
#include <folly/AtomicHashMap.h>
#include <folly/FBVector.h>

#include "backend.h"
#include "config.h"
#include "extmap.h"
#include "journal.h"
#include "read_cache.h"
#include "smartiov.h"
#include "utils.h"

template <typename T> using FutRes = folly::Future<Result<T>>;

struct SuperblockInfo {
    u64 magic = LSVD_MAGIC;
    u64 version = 1;
    u64 block_size = 4096;
    u64 image_size;

    std::map<seqnum_t, std::string> clones;
    std::vector<seqnum_t> checkpoints;
    std::vector<seqnum_t> snapshots;
};

class LogObj;

class LsvdImage
{
    const usize rollover_threshold = 8 * 1024 * 1024;
    const usize max_log_size = rollover_threshold * 2;
    const usize block_size = 4096;
    const usize checkpoint_interval_epoch = 100;

  public:
    const str name;

  private:
    // Cannot be copied or moved
    LsvdImage(LsvdImage &) = delete;
    LsvdImage(LsvdImage &&) = delete;
    LsvdImage operator=(LsvdImage &) = delete;
    LsvdImage operator=(LsvdImage &&) = delete;

    // Clone, checkpoint, and snapshopt metadata
    SuperblockInfo superblock;
    seqnum_t last_checkpoint;

    // Utilities
    LsvdConfig cfg;
    ExtMap extmap;
    uptr<ObjStore> s3;
    uptr<ReadCache> cache;
    uptr<Journal> journal;

    // The "log" part of LSVD
    folly::coro::SharedMutex logobj_mtx;
    sptr<LogObj> cur_logobj;
    folly::coro::SharedMutex pending_mtx;
    folly::F14FastMap<seqnum_t, sptr<LogObj>> pending_objs;

    // Internal functions
    std::string get_key(seqnum_t seqnum);
    Task<sptr<LogObj>> log_rollover(bool force);
    Task<void> flush_logobj(sptr<LogObj> obj);
    Task<void> checkpoint(seqnum_t seqnum);

  public:
    static Result<uptr<LsvdImage>> mount(sptr<ObjStore> s3, str name,
                                         str config);
    void unmount();

    static FutRes<void> create(sptr<ObjStore> s3, str name);
    static FutRes<void> remove(sptr<ObjStore> s3, str name);
    static FutRes<void> clone(sptr<ObjStore> s3, str src, str dst);

    ResTask<void> read(off_t offset, smartiov iovs);
    ResTask<void> write(off_t offset, smartiov iovs);
    ResTask<void> trim(off_t offset, usize len);
    ResTask<void> flush();
};