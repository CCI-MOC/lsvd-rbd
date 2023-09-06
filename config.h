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

#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <string>
#include <uuid/uuid.h>

enum cfg_backend { BACKEND_FILE = 1, BACKEND_RADOS = 2 };

enum cfg_cache_type { LSVD_CFG_READ = 1, LSVD_CFG_WRITE = 2 };

class lsvd_config
{
  public:
    int batch_size = 8 * 1024 * 1024;   // in bytes
    int wcache_batch = 8;               // requests
    int wcache_chunk = 2 * 1024 * 1024; // bytes
    std::string rcache_dir = "/tmp";
    std::string wcache_dir = "/tmp";
    int xlate_window = 20;
    int hard_sync = 0;
    enum cfg_backend backend = BACKEND_RADOS;
    long cache_size = 500 * 1024 * 1024; // in bytes
    int ckpt_interval = 500;             // objects
    int flush_msec = 2000;               // flush timeout
    int gc_threshold = 60;               // GC threshold, percent
    int fetch_window = 12;               // read cache fetches
    int fetch_ratio = 67;                // anti-thrash ratio, percent

    lsvd_config() {}
    ~lsvd_config() {}
    int read();
    std::string cache_filename(uuid_t &uuid, const char *name, cfg_cache_type type);
};

#endif
