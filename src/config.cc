#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <uuid/uuid.h>

#include "config.h"
#include "utils.h"

lsvd_config lsvd_config::get_default() { return lsvd_config(); }

result<lsvd_config> lsvd_config::from_user_cfg(str cfg)
{
    auto c = get_default();
    if (cfg.empty())
        return c;

    BOOST_OUTCOME_TRYX(c.parse_file("/usr/local/etc/lsvd.json", true));
    BOOST_OUTCOME_TRYX(c.parse_file("./lsvd.json", true));

    if (cfg[0] == '{') {
        BOOST_OUTCOME_TRYX(c.parse_json(cfg));
    } else {
        BOOST_OUTCOME_TRYX(c.parse_file(cfg, false));
    }
    return c;
}

// https://stackoverflow.com/questions/116038/how-do-i-read-an-entire-file-into-a-stdstring-in-c
auto read_file(std::string_view path) -> result<str>
{
    constexpr auto read_size = std::size_t(4096);
    auto stream = std::ifstream(path.data());
    stream.exceptions(std::ios_base::badbit);

    if (not stream)
        return outcome::failure(std::errc::no_such_file_or_directory);

    auto out = std::string();
    auto buf = std::string(read_size, '\0');
    while (stream.read(&buf[0], read_size))
        out.append(buf, 0, stream.gcount());
    out.append(buf, 0, stream.gcount());
    return out;
}

auto lsvd_config::parse_file(str path, bool allow_missing) -> result<void>
{
    assert(!path.empty());

    auto cfg = read_file(path);
    if (cfg.has_error()) {
        if (cfg.error() == std::errc::no_such_file_or_directory &&
            allow_missing) {
            return outcome::success();
        } else {
            log_error("Failed to read config file '{}': {}", path,
                      cfg.error().message());
            return cfg.as_failure();
        }
    }

    BOOST_OUTCOME_TRYX(parse_json(cfg.value()));
    return outcome::success();
}

#define JSON_GET_BOOL(key)                                                     \
    do {                                                                       \
        if (js.contains(#key)) {                                               \
            auto v = js[#key];                                                 \
            if (v.is_boolean()) {                                              \
                this->key = js[#key];                                          \
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
                this->key = js[#key];                                          \
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
                this->key = js[#key];                                          \
            } else {                                                           \
                log_error("Invalid value for key (must be str): {}", #key);    \
            }                                                                  \
        }                                                                      \
    } while (0)

result<void> lsvd_config::parse_json(str json)
{
    auto js = nlohmann::json::parse(json);

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

    return outcome::success();
}
