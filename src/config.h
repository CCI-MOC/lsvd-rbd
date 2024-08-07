#pragma once

/*
 * file:        config.h
 * description: quick and dirty config file parser
 *              env var overrides modeled on github.com/spf13/viper
 *
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <string>
#include <uuid/uuid.h>

#include "utils.h"

enum cfg_backend { BACKEND_FILE = 1, BACKEND_RADOS = 2 };

enum cfg_cache_type { LSVD_CFG_READ = 1, LSVD_CFG_WRITE = 2 };

class lsvd_config
{
  public:
    int backend_obj_size = 8 * 1024 * 1024; // in bytes
    int wcache_batch = 8;                   // requests
    int wcache_chunk = 2 * 1024 * 1024;     // bytes
    std::string rcache_dir = "/tmp/lsvd/";
    std::string wcache_dir = "/tmp/lsvd/";
    u32 num_parallel_writes = 8;
    int hard_sync = 0;
    enum cfg_backend backend = BACKEND_RADOS;
    long cache_size = 500 * 1024 * 1024; // in bytes
    long wlog_size = 500 * 1024 * 1024;  // in bytes
    int ckpt_interval = 500;             // objects
    int flush_timeout_msec = 2000;       // flush timeout
    int flush_interval_msec = 1000;      // flush interval
    int gc_threshold = 60;               // GC threshold, percent
    int gc_window = 4;                   // max GC writes outstanding
    int fetch_window = 12;               // read cache fetches
    int fetch_ratio = 67;                // anti-thrash served:backend ratio
    int no_gc = 0;                       // turn off GC

    lsvd_config() {}
    ~lsvd_config() {}
    int read();
    str cache_filename(uuid_t &uuid, const char *name, cfg_cache_type type);

    inline fspath wlog_path(str imgname)
    {
        auto filename = imgname + ".wlog";
        return fspath(wcache_dir) / filename;
    }

    static opt<lsvd_config> from_file(str path);
};
