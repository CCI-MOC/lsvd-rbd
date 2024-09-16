#pragma once

#include <boost/outcome.hpp>
#include <boost/stacktrace.hpp>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <folly/experimental/coro/Promise.h>
#include <folly/experimental/coro/Task.h>
#include <future>
#include <linux/fs.h>
#include <optional>
#include <stdio.h>
#include <string>
#include <vector>

namespace outcome = boost::outcome_v2;
template <typename T> using Result = outcome::result<T>;
template <typename T> using Task = folly::coro::Task<T>;
template <typename T> using ResTask = folly::coro::Task<outcome::result<T>>;

template <typename T> using sptr = std::shared_ptr<T>;
template <typename T> using uptr = std::unique_ptr<T>;
template <typename T> using opt = std::optional<T>;
template <typename T> using vec = std::vector<T>;
template <typename T> using fut = std::future<T>;
// template <typename T> using fvec = folly::fbvector<T>;

using fspath = std::filesystem::path;

#define log_trace(MSG, ...)                                                    \
    do {                                                                       \
        if (LOGLV <= 0)                                                        \
            fmt::print(stderr, "[TRC {}:{} {}] " MSG "\n", __FILE__, __LINE__, \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define log_debug(MSG, ...)                                                    \
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
