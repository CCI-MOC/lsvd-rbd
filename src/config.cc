/*
 * file:        config.cc
 * description: quick and dirty config file parser
 *              env var overrides modeled on github.com/spf13/viper
 *
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
namespace fs = std::filesystem;

#include "config.h"
#include "config_macros.h"

vec<std::string> cfg_path({"lsvd.conf", "/usr/local/etc/lsvd.conf"});

static void split(std::string s, vec<std::string> &words)
{
    std::string w = "";
    for (auto c : s) {
        if (!isspace(c))
            w = w + c;
        else {
            words.push_back(w);
            w = "";
        }
    }
    if (w.size() > 0)
        words.push_back(w);
}

static std::map<std::string, cfg_backend> m = {{"file", BACKEND_FILE},
                                               {"rados", BACKEND_RADOS}};

/* fancy-ass macros to parse config file lines.
 *   config keyword = field name
 *   environment var = LSVD_ + uppercase(field name)
 *   skips blank lines and lines that don't match a keyword
 */

int lsvd_config::read()
{
    auto explicit_cfg = getenv("LSVD_CONFIG_FILE");
    if (explicit_cfg) {
        std::string f(explicit_cfg);
        cfg_path.insert(cfg_path.begin(), f);
    }
    for (auto f : cfg_path) {
        std::ifstream fp(f);
        if (!fp.is_open())
            continue;
        std::string line;
        while (getline(fp, line)) {
            if (line[0] == '#')
                continue;
            vec<std::string> words;
            split(line, words);
            if (words.size() != 2)
                continue;
            F_CONFIG_H_INT(words[0], words[1], backend_obj_size);
            F_CONFIG_INT(words[0], words[1], wcache_batch);
            F_CONFIG_H_INT(words[0], words[1], wcache_chunk);
            F_CONFIG_STR(words[0], words[1], rcache_dir);
            F_CONFIG_STR(words[0], words[1], wcache_dir);
            F_CONFIG_INT(words[0], words[1], num_parallel_writes);
            F_CONFIG_TABLE(words[0], words[1], backend, m);
            F_CONFIG_H_INT(words[0], words[1], cache_size);
            F_CONFIG_H_INT(words[0], words[1], wlog_size);
            F_CONFIG_INT(words[0], words[1], hard_sync);
            F_CONFIG_INT(words[0], words[1], ckpt_interval);
            F_CONFIG_INT(words[0], words[1], flush_timeout_msec);
            F_CONFIG_INT(words[0], words[1], gc_threshold);
            F_CONFIG_INT(words[0], words[1], fetch_window);
            F_CONFIG_INT(words[0], words[1], fetch_ratio);
            F_CONFIG_INT(words[0], words[1], no_gc);
            F_CONFIG_INT(words[0], words[1], gc_window);
        }
        fp.close();
        break;
    }

    ENV_CONFIG_H_INT(backend_obj_size);
    ENV_CONFIG_INT(wcache_batch);
    ENV_CONFIG_H_INT(wcache_chunk);
    ENV_CONFIG_STR(rcache_dir);
    ENV_CONFIG_STR(wcache_dir);
    ENV_CONFIG_INT(num_parallel_writes);
    ENV_CONFIG_TABLE(backend, m);
    ENV_CONFIG_H_INT(cache_size);
    ENV_CONFIG_H_INT(wlog_size);
    ENV_CONFIG_INT(hard_sync);
    ENV_CONFIG_INT(ckpt_interval);
    ENV_CONFIG_INT(flush_timeout_msec);
    ENV_CONFIG_INT(gc_threshold);
    ENV_CONFIG_INT(fetch_window);
    ENV_CONFIG_INT(fetch_ratio);
    ENV_CONFIG_INT(no_gc);
    ENV_CONFIG_INT(gc_window);

    return 0; // success
}

std::string lsvd_config::cache_filename(uuid_t &uuid, const char *name,
                                        cfg_cache_type type)
{
    char buf[256]; // PATH_MAX
    std::string file(name);
    file = fs::path(file).filename();
    const char *dir;
    const char *f_ext;

    dir = (type == LSVD_CFG_READ) ? rcache_dir.c_str() : wcache_dir.c_str();
    f_ext = (type == LSVD_CFG_READ) ? "rcache" : "wcache";

    sprintf(buf, "%s/%s.%s", dir, file.c_str(), f_ext);
    if (access(buf, R_OK | W_OK) == 0)
        return std::string((const char *)buf);

    char uuid_s[64];
    uuid_unparse(uuid, uuid_s);
    sprintf(buf, "%s/%s.%s", dir, uuid_s, f_ext);
    return std::string((const char *)buf);
}

#if 0
int main(int argc, char **argv) {
    auto cfg = new lsvd_config;
    cfg->read();

    printf("batch: %d\n", cfg->batch_size); // h_int
    printf("wc batch %d\n", cfg->wcache_batch); // int
    printf("cache: %s\n", cfg->cache_dir.c_str()); // str
    printf("backend: %d\n", (int)cfg->backend);	   // table

    uuid_t uu;
    std::cout << cfg->cache_filename(uu, "foobar") << "\n";
}
#endif
