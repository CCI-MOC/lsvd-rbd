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

    Result<void> deserialise(vec<byte> buf)
    {
        zpp::bits::in ar(buf);
        auto res = ar(*this);
        if (zpp::bits::failure(res)) {
            XLOGF(ERR, "Failed to deserialise superblock");
            return outcome::failure(res.code);
        }
        return outcome::success();
    }

    Result<vec<byte>> serialise()
    {
        vec<byte> buf;
        buf.reserve(4096);
        zpp::bits::out ar(buf);
        auto res = ar(*this);
        if (zpp::bits::failure(res)) {
            XLOGF(ERR, "Failed to serialise superblock");
            return outcome::failure(res.code);
        }
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
    const usize rollover_threshold = max_log_size - (2 * max_io_size);
    const usize sector_size = 512;
    const usize checkpoint_interval_epoch = 128;
    const usize max_recycle_objs = 32;

  public:
    const fstr name;

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

    folly::Synchronized<vec<sptr<LogObj>>> recycle_objs;

    // Internal functions
    Task<sptr<LogObj>> rollover_log(bool force);
    ResTask<void> flush_logobj(sptr<LogObj> obj);
    ResTask<void> checkpoint(seqnum_t seqnum, vec<byte> buf);
    ResTask<void> replay_obj(seqnum_t seq, vec<byte> buf, usize start_byte);

    // debug utils to maintain consistency and integrity
    folly::Synchronized<folly::F14FastMap<seqnum_t, usize>> obj_sizes;

  public:
    static ResTask<uptr<LsvdImage>> mount(sptr<ObjStore> s3, fstr name,
                                          fstr config);
    Task<void> unmount();

    static ResTask<void> create(sptr<ObjStore> s3, fstr name, usize size);
    static ResTask<void> remove(sptr<ObjStore> s3, fstr name);
    static ResTask<void> clone(sptr<ObjStore> s3, fstr src, fstr dst);

    ResTask<void> read(off_t offset, smartiov iovs);
    ResTask<void> write(off_t offset, smartiov iovs);
    ResTask<void> write_and_verify(off_t offset, smartiov iovs);
    ResTask<void> trim(off_t offset, usize len);
    ResTask<void> flush();

    Task<void> verify_integrity();
    usize get_size() { return superblock.image_size; }
};