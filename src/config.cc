#include <nlohmann/json.hpp>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <uuid/uuid.h>

#include "config.h"
#include "utils.h"

vec<std::string> cfg_path({"lsvd.conf", "/usr/local/etc/lsvd.conf"});

opt<lsvd_config> lsvd_config::from_file(str path)
{
    // write just a simple braindead parser
    // syntax: lines of `key = value`

    UNIMPLEMENTED();
}

lsvd_config lsvd_config::get_default() { return lsvd_config(); }

#define JSON_GET_BOOL(key)                                                     \
    do {                                                                       \
        if (js.contains(#key)) {                                               \
            auto v = js[#key];                                                 \
            if (v.is_boolean()) {                                              \
                cfg.key = js[#key];                                            \
            } else {                                                           \
                log_error("Invalid value for key (must be bool): {}", #key);   \
            }                                                                  \
        }                                                                      \
    } while (0)

#define JSON_GET_UINT(key)                                                     \
    do {                                                                       \
        if (js.contains(#key)) {                                               \
            auto v = js[#key];                                                 \
            if (v.is_number_unsigned()) {                                      \
                cfg.key = js[#key];                                            \
            } else {                                                           \
                log_error("Invalid value for key (must be uint): {}", #key);   \
            }                                                                  \
        }                                                                      \
    } while (0)

#define JSON_GET_STR(key)                                                      \
    do {                                                                       \
        if (js.contains(#key)) {                                               \
            auto v = js[#key];                                                 \
            if (v.is_string()) {                                               \
                cfg.key = js[#key];                                            \
            } else {                                                           \
                log_error("Invalid value for key (must be str): {}", #key);    \
            }                                                                  \
        }                                                                      \
    } while (0)

opt<lsvd_config> lsvd_config::from_json(str json)
{
    nlohmann::json js;

    try {
        js = nlohmann::json::parse(json);
    } catch (nlohmann::json::parse_error &e) {
        log_error("Failed to parse JSON: {}", e.what());
        return std::nullopt;
    }

    lsvd_config cfg;
    JSON_GET_STR(rcache_dir);
    JSON_GET_UINT(rcache_bytes);
    JSON_GET_UINT(rcache_fetch_window);

    JSON_GET_STR(wlog_dir);
    JSON_GET_UINT(wlog_bytes);
    JSON_GET_UINT(wlog_write_window);
    JSON_GET_UINT(wlog_chunk_bytes);

    JSON_GET_UINT(antithrash_ratio);
    JSON_GET_UINT(backend_obj_bytes);
    JSON_GET_UINT(backend_write_window);
    JSON_GET_UINT(checkpoint_interval_objs);
    JSON_GET_UINT(flush_timeout_ms);
    JSON_GET_UINT(flush_interval_ms);

    JSON_GET_UINT(gc_threshold_pc);
    JSON_GET_UINT(gc_write_window);
    JSON_GET_BOOL(no_gc);

    return cfg;
}
