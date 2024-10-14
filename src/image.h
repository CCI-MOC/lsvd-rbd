#include <boost/uuid/uuid.hpp>
#include <folly/AtomicHashMap.h>
#include <folly/FBVector.h>
#include <zpp_bits.h>

#include "backend.h"
#include "config.h"
#include "extmap.h"
#include "journal.h"
#include "read_cache.h"
#include "smartiov.h"
#include "utils.h"

const u32 LOG_REPLAY_OBJECT_COUNT = 16;

template <typename T> using FutRes = folly::Future<Result<T>>;

struct SuperblockInfo {
    u64 magic = LSVD_MAGIC;
    u64 version = 1;
    u64 block_size = 4096;
    u64 image_size;

    std::map<seqnum_t, std::string> clones;
    std::vector<seqnum_t> checkpoints;
    std::vector<seqnum_t> snapshots;

    static SuperblockInfo deserialise(vec<byte> &buf)
    {
        SuperblockInfo ret;
        zpp::bits::in ar(buf);
        ar(ret).or_throw();
        return ret;
    }

    Result<vec<byte>> serialise()
    {
        vec<byte> buf;
        buf.reserve(4096);
        zpp::bits::out ar(buf);
        ar(*this).or_throw();
        return buf;
    }
};

class LogObj;

class LsvdImage
{
  public:
    const usize max_io_size = 128 * 1024;

  private:
    const usize max_log_size = 8 * 1024 * 1024;
    const usize rollover_threshold = 2 * max_io_size;
    const usize sector_size = 512;
    const usize checkpoint_interval_epoch = 128;
    const usize max_recycle_objs = 32;

  public:
    const fstr name;

    ~LsvdImage() { XLOGF(INFO, "Destructing image '{}'", name); }

  private:
    LsvdImage(fstr name) : name(name) {}

    // Cannot be copied or moved
    LsvdImage(LsvdImage &) = delete;
    LsvdImage(LsvdImage &&) = delete;
    LsvdImage operator=(LsvdImage &) = delete;
    LsvdImage operator=(LsvdImage &&) = delete;

    // Clone, checkpoint, and snapshopt metadata
    SuperblockInfo superblock;
    std::atomic<seqnum_t> last_checkpoint;

    // Utilities
    LsvdConfig cfg;
    uptr<ExtMap> extmap;
    sptr<ObjStore> s3;
    uptr<ReadCache> cache;
    uptr<Journal> journal;

    // The "log" part of LSVD
    folly::coro::SharedMutex logobj_mtx;
    sptr<LogObj> cur_logobj;
    folly::coro::SharedMutex pending_mtx;
    folly::F14FastMap<seqnum_t, sptr<LogObj>> pending_objs;

    folly::coro::SharedMutex recycle_mtx;
    fvec<sptr<LogObj>> recycle_objs;

    // Internal functions
    Task<sptr<LogObj>> rollover_log(bool force);
    TaskUnit flush_logobj(sptr<LogObj> obj);
    TaskUnit checkpoint(seqnum_t seqnum, vec<byte> buf);
    TaskUnit replay_obj(seqnum_t seq, vec<byte> buf, usize start_byte);

    // debug utils to maintain consistency and integrity
    folly::Synchronized<folly::F14FastMap<seqnum_t, usize>> obj_sizes;

  public:
    static TaskRes<uptr<LsvdImage>> mount(sptr<ObjStore> s3, fstr name,
                                          fstr config);
    Task<void> unmount();

    static TaskUnit create(sptr<ObjStore> s3, fstr name, usize size);
    static TaskUnit remove(sptr<ObjStore> s3, fstr name);
    static TaskUnit clone(sptr<ObjStore> s3, fstr src, fstr dst);

    TaskUnit read(off_t offset, smartiov iovs);
    TaskUnit write(off_t offset, smartiov iovs);
    TaskUnit trim(off_t offset, usize len);
    TaskUnit flush();

    TaskUnit write_and_verify(off_t offset, smartiov iovs);
    Task<void> verify_integrity();
    usize get_size() { return superblock.image_size; }
};