#pragma once

// #include "folly/FBVector.h"
#include <boost/stacktrace.hpp>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <iostream>
#include <linux/fs.h>
#include <mutex>
#include <optional>
#include <queue>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <stdio.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <vector>

#ifndef LOGLV
#define LOGLV 1
#endif

template <typename T> using sptr = std::shared_ptr<T>;
template <typename T> using uptr = std::unique_ptr<T>;
template <typename T> using opt = std::optional<T>;
template <typename T> using vec = std::vector<T>;
// template <typename T> using fvec = folly::fbvector<T>;

#define CEXTERN extern "C"

using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;
using u8 = uint8_t;
using s64 = int64_t;
using s32 = int32_t;
using s16 = int16_t;
using s8 = int8_t;
using usize = size_t;
using ssize = ssize_t;
using byte = std::byte;
using str = std::string;
using fspath = std::filesystem::path;

#define PASSTHRU_NULLOPT(opt)                                                  \
    do {                                                                       \
        if (!opt) {                                                            \
            return std::nullopt;                                               \
        }                                                                      \
    } while (0)

#define PASSTHRU_NULLPTR(ptr)                                                  \
    do {                                                                       \
        if (!ptr) {                                                            \
            return nullptr;                                                    \
        }                                                                      \
    } while (0)

#define trace(MSG, ...)                                                        \
    do {                                                                       \
        if (LOGLV <= 0)                                                        \
            fmt::print(stderr, "[TRC {}:{} {}] " MSG "\n", __FILE__, __LINE__, \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define debug(MSG, ...)                                                        \
    do {                                                                       \
        if (LOGLV <= 1)                                                        \
            fmt::print(stderr, fg(fmt::terminal_color::magenta),               \
                       "[DBG {}:{} {}] " MSG "\n", __FILE__, __LINE__,         \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define log_info(MSG, ...)                                                     \
    do {                                                                       \
        if (LOGLV <= 2)                                                        \
            fmt::print(stderr,                                                 \
                       fg(fmt::terminal_color::cyan) | fmt::emphasis::bold,    \
                       "[INFO {}:{} {}] " MSG "\n", __FILE__, __LINE__,        \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define log_warn(MSG, ...)                                                     \
    do {                                                                       \
        if (LOGLV <= 3)                                                        \
            fmt::print(stderr,                                                 \
                       fg(fmt::terminal_color::yellow) | fmt::emphasis::bold,  \
                       "[WARN {}:{} {}] " MSG "\n", __FILE__, __LINE__,        \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define log_error(MSG, ...)                                                    \
    do {                                                                       \
        fmt::print(stderr, fg(fmt::terminal_color::red) | fmt::emphasis::bold, \
                   "[ERR {}:{} {}] " MSG "\n", __FILE__, __LINE__, __func__,   \
                   ##__VA_ARGS__);                                             \
    } while (0)

#define trap_to_debugger()                                                     \
    do {                                                                       \
        raise(SIGTRAP);                                                        \
    } while (0)

#define RET_IF(cond, ret)                                                      \
    do {                                                                       \
        if (cond) {                                                            \
            return ret;                                                        \
        }                                                                      \
    } while (0)

#define PR_RET_IF(cond, ret, MSG, ...)                                         \
    do {                                                                       \
        if (cond) {                                                            \
            log_error(MSG, ##__VA_ARGS__);                                     \
            return ret;                                                        \
        }                                                                      \
    } while (0)

#define PR_GOTO_IF(cond, lbl, MSG, ...)                                        \
    do {                                                                       \
        if (cond) {                                                            \
            log_error(MSG, ##__VA_ARGS__);                                     \
            goto lbl;                                                          \
        }                                                                      \
    } while (0)

/**
 * If `cond` is true, print an error message to stdout with MSG, then return
 * `ret`
 */
#define PR_ERR_RET_IF(cond, ret, en, MSG, ...)                                 \
    do {                                                                       \
        if (cond) {                                                            \
            auto fs = fmt::format(MSG "\n", ##__VA_ARGS__);                    \
            auto s = fmt::format("[ERR {}:{} {} | errno {}/{}] {}", __FILE__,  \
                                 __LINE__, __func__, en, strerror(en), fs);    \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold, s);  \
            return ret;                                                        \
        }                                                                      \
    } while (0)

#define THROW_MSG_ON(cond, MSG, ...)                                           \
    do {                                                                       \
        if (cond) {                                                            \
            auto m = fmt::format(MSG, ##__VA_ARGS__);                          \
            auto s = fmt::format("[ERR {}:{} {}] {}\n", __FILE__, __LINE__,    \
                                 __func__, m);                                 \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold, s);  \
            throw std::runtime_error(m);                                       \
        }                                                                      \
    } while (0)

#define THROW_ERRNO_ON(cond, en, MSG, ...)                                     \
    do {                                                                       \
        if (cond) {                                                            \
            auto m =                                                           \
                fmt::format("{}/{}: " MSG, en, strerror(en), ##__VA_ARGS__);   \
            throw std::system_error(en, std::generic_category(), m);           \
        }                                                                      \
    } while (0)

/**
 * Check return values of libstdc functions. If it's -1, print the error and
 * throw an exception
 */
#define check_ret_errno(ret, MSG, ...)                                         \
    do {                                                                       \
        if (ret == -1) {                                                       \
            auto en = errno;                                                   \
            auto fs = fmt::format(MSG "\n", ##__VA_ARGS__);                    \
            auto s = fmt::format("[ERR {}:{} {} | errno {}/{}] {}", __FILE__,  \
                                 __LINE__, __func__, en, strerror(en), fs);    \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold, s);  \
            throw std::runtime_error(fs);                                      \
        }                                                                      \
    } while (0)

/**
 * Check return values of functions that return -errno in the case of an error
 * and throw an exception
 */
#define check_ret_neg(ret, MSG, ...)                                           \
    do {                                                                       \
        if (ret < 0) {                                                         \
            auto en = -ret;                                                    \
            auto fs = fmt::format(MSG "\n", ##__VA_ARGS__);                    \
            auto s = fmt::format("[ERR {}:{} {} | errno {}/{}] {}", __FILE__,  \
                                 __LINE__, __func__, en, strerror(en), fs);    \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold, s);  \
            throw std::runtime_error(fs);                                      \
        }                                                                      \
    } while (0)

#define check_cond(cond, MSG, ...)                                             \
    do {                                                                       \
        if (cond) {                                                            \
            auto s = fmt::format("[ERR {}:{} {}]: " MSG "\n", __FILE__,        \
                                 __LINE__, __func__, ##__VA_ARGS__);           \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold, s);  \
            throw std::runtime_error(s);                                       \
        }                                                                      \
    } while (0)

#define TODO()                                                                 \
    do {                                                                       \
        fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold,          \
                   "[ERR {}:{} {}] TODO\n", __FILE__, __LINE__, __func__);     \
        throw std::runtime_error("TODO stub");                                 \
    } while (0)

#define UNIMPLEMENTED()                                                        \
    do {                                                                       \
        fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold,          \
                   "[ERR {}:{} {}] Unimplemented\n", __FILE__, __LINE__,       \
                   __func__);                                                  \
        throw std::runtime_error("Unimplemented");                             \
    } while (0)

template <class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};

inline vec<std::string> split_string_on_char(const std::string &s, char delim)
{
    vec<std::string> result;
    std::stringstream ss(s);
    std::string item;

    while (getline(ss, item, delim)) {
        result.push_back(item);
    }

    return result;
}

inline std::string string_join(const vec<std::string> &strings,
                               const std::string &delim)
{
    std::string result;
    for (auto it = strings.begin(); it != strings.end(); it++) {
        result += *it;
        if (it != strings.end() - 1)
            result += delim;
    }
    return result;
}

template <typename T> std::shared_ptr<T> to_shared(std::unique_ptr<T> ptr)
{
    return std::shared_ptr<T>(std::move(ptr));
}

inline bool has_poolname_prefix(const std::string &s)
{
    auto idx = s.find("/");
    return idx != std::string::npos;
}

inline char *strip_poolname_prefix(char *s)
{
    auto idx = strchr(s, '/');
    if (idx == nullptr)
        return s;
    return idx + 1;
}

inline size_t getsize64(int fd)
{
    struct stat sb;
    size_t size;

    if (fstat(fd, &sb) < 0)
        throw std::runtime_error("fstat");
    if (S_ISBLK(sb.st_mode)) {
        if (ioctl(fd, BLKGETSIZE64, &size) < 0)
            throw std::runtime_error("ioctl");
    } else
        size = sb.st_size;
    return size;
}
