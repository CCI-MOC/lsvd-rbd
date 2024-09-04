#pragma once

#include <uuid/uuid.h>

#include "utils.h"

class lsvd_config
{
  public:
    // read cache
    str rcache_dir = "/tmp/lsvd/";        // directory to store read cache files
    u64 rcache_bytes = 500 * 1024 * 1024; // in bytes
    u64 rcache_fetch_window = 12;         // read cache fetches

    // write log
    str wlog_dir = "/tmp/lsvd/";
    u64 wlog_bytes = 500 * 1024 * 1024;     // in bytes
    u64 wlog_write_window = 8;              // requests
    u64 wlog_chunk_bytes = 2 * 1024 * 1024; // bytes

    // backend
    u64 antithrash_ratio = 67;               // anti-thrash served:backend ratio
    u64 backend_obj_bytes = 8 * 1024 * 1024; // in bytes
    u64 backend_write_window = 8;            // backend parallel writes

    u64 checkpoint_interval_objs = 500; // objects
    u64 flush_timeout_ms = 2000;        // flush timeout
    u64 flush_interval_ms = 1000;       // flush interval

    // GC
    u64 gc_threshold_pc = 60; // GC threshold, percent
    u64 gc_write_window = 4;  // max GC writes outstanding
    bool no_gc = false;       // turn off GC

    ~lsvd_config() {}

    inline fspath wlog_path(str imgname)
    {
        auto filename = imgname + ".wlog";
        return fspath(wlog_dir) / filename;
    }

    static lsvd_config get_default();

    /**
     * Read LSVD configuration from user-supplied string. The string can be
     * either a json string containing the configuration, the path to a file
     * containing the same, or empty, in which case it will be ignored.
     */
    static Result<lsvd_config> from_user_cfg(str cfg);

  private:
    lsvd_config() {}

    Result<void> parse_json(str js);
    Result<void> parse_file(str path, bool can_be_missing = true);
};
