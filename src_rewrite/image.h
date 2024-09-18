#include <folly/AtomicHashMap.h>
#include <folly/FBVector.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <uuid.h>

#include "backend.h"
#include "config.h"
#include "extmap.h"
#include "journal.h"
#include "read_cache.h"
#include "smartiov.h"
#include "utils.h"

template <typename T> using FutRes = folly::Future<Result<T>>;

struct SuperblockInfo {
    u64 magic;
    u64 version;
    u64 block_size_bytes;
    u64 image_size_bytes;

    folly::fbvector<seqnum_t> checkpoints;
    folly::fbvector<seqnum_t> snapshots;
    folly::AtomicHashMap<seqnum_t, str> clones;
};

class LogObj;

class LsvdImage
{
    const usize rollover_threshold = 4 * 1024 * 1024;  // 4MB
    const usize max_log_size = rollover_threshold * 2; // 8MB
    const usize block_size = 4096;

  public:
    const str name;
    const uuid_t uuid;
    const usize size_bytes;

  private:
    folly::Executor::KeepAlive<> &executor;
    LsvdConfig cfg;
    ExtMap extmap;
    sptr<ObjStore> s3;
    sptr<ReadCache> cache;
    sptr<Journal> journal;

    // The "log" part of LSVD
    folly::coro::SharedMutex logobj_mtx;
    sptr<LogObj> cur_logobj;
    folly::coro::SharedMutex pending_mtx;
    folly::F14FastMap<seqnum_t, sptr<LogObj>> pending_objs;

    // Clone, checkpoint, and snapshopt metadata
    SuperblockInfo info;

    // Internal functions
    std::string get_key(seqnum_t seqnum);
    Task<sptr<LogObj>> log_rollover(bool force);
    Task<void> flush_logobj(sptr<LogObj> obj);
    Task<void> checkpoint();

    // Cannot be copied or moved
    LsvdImage(LsvdImage &) = delete;
    LsvdImage(LsvdImage &&) = delete;
    LsvdImage operator=(LsvdImage &) = delete;
    LsvdImage operator=(LsvdImage &&) = delete;

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