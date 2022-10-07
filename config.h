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

enum cfg_backend { BACKEND_FILE = 1, BACKEND_RADOS = 2 };

class config {
public:

    int         batch_size = 8*1024*1024; // in bytes
    int         wcache_window = 32;	  // in operations
    std::string cache_dir;
    int         wcache_threads = 2;
    int         xlate_threads = 2;
    enum cfg_backend backend;
    
    config(){}
    ~config(){ }
    void read();
};

#endif
