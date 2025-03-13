#pragma once
#include <filesystem>

#include "folly/portability/GFlags.h"
#include "representation.h"
#include "utils.h"

const s64 MS_TO_NS = 1'000'000;
const usize MIB = 1024 * 1024;
const usize GIB = 1024 * 1024 * 1024;

const bool LSVD_IS_NOOP = false;

const bool ENABLE_SEQUENTIAL_DEBUG_READS = false;
const bool VERIFY_MAP_INTEGRITY_ON_UPDATE = false;
const bool ENABLE_JOURNAL = true;
const bool ENABLE_FLUSH = true;
FOLLY_GFLAGS_DECLARE_bool(lsvd_report_long_ops);
FOLLY_GFLAGS_DECLARE_bool(lsvd_report_cache_stats);
FOLLY_GFLAGS_DECLARE_bool(lsvd_report_iotiming);

const s64 LONG_READ_NS_THRES = 1 * MS_TO_NS;
const s64 LONG_WRITE_NS_THRES = 50 * MS_TO_NS;
const s64 LONG_URING_NS_THRES = 20 * MS_TO_NS;

class LsvdConfig
{
  public:
    u64 journal_bytes = 1 * GIB;

    bool gc_enable = false;
    f64 gc_live_ratio = 0.666;

    bool checkpoint_enable = true;
    f64 cache_antithrash_ratio = 0.666;

    u32 max_backend_ios = 16;
    bool write_to_cache = true;

    auto to_string()
    {
        return fmt::format("LsvdConfig: journal_bytes={}\n"
                           "gc_enable={}\n"
                           "gc_live_ratio={}\n"
                           "checkpoint_enable={}\n"
                           "cache_antithrash_ratio={}\n",
                           "write_to_cache={}", journal_bytes, gc_enable,
                           gc_live_ratio, checkpoint_enable,
                           cache_antithrash_ratio, write_to_cache);
    }

    static Result<LsvdConfig> parse(str imgname, str cfg_str);
};
