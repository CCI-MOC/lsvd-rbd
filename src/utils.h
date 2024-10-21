#pragma once
#include "absl/status/status.h"
#include "folly/Unit.h"
#include <absl/status/statusor.h>
#include <boost/stacktrace.hpp>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fmt/color.h>
#include <folly/experimental/coro/Promise.h>
#include <folly/experimental/coro/Task.h>
#include <folly/logging/xlog.h>
#include <future>
#include <linux/fs.h>
#include <optional>
#include <source_location>
#include <vector>

template <typename T> using Task = folly::coro::Task<T>;
template <typename T> using Result = absl::StatusOr<T>;
template <typename T> using ResVoid = Result<folly::Unit>;
template <typename T> using TaskRes = Task<Result<T>>;
using TaskUnit = TaskRes<folly::Unit>;

template <typename T> using sptr = std::shared_ptr<T>;
template <typename T> using uptr = std::unique_ptr<T>;
template <typename T> using opt = std::optional<T>;
template <typename T> using vec = std::vector<T>;
template <typename T> using fut = std::future<T>;
template <typename T> using fvec = folly::fbvector<T>;

using fspath = std::filesystem::path;
using str = std::string;
using strv = std::string_view;
using strc = const std::string &;

#define LOGERR_AND_RET_IF_FAIL(res, msg, ...)                                  \
    do {                                                                       \
        if (!res.ok()) [[unlikely]] {                                          \
            auto outmsg = fmt::format(msg, __VA_ARGS__);                       \
            XLOGF(ERR, "{}: {}", outmsg, res.status().ToString());             \
            return res.status();                                               \
        }                                                                      \
    } while (0)

#define ENSURE(cond)                                                           \
    do {                                                                       \
        if (!(cond)) [[unlikely]] {                                            \
            auto msg = fmt::format("Assertion failed: '{}'", #cond);           \
            XLOG(ERR, msg);                                                    \
            throw std::runtime_error(msg);                                     \
        }                                                                      \
    } while (0)

#define CRET_IF_NOTOK(stat)                                                    \
    do {                                                                       \
        if (!stat.ok()) [[unlikely]] {                                         \
            co_return stat.status();                                           \
        }                                                                      \
    } while (0)

#define DEBUG_IF_FAIL(stat)                                                    \
    do {                                                                       \
        if (!stat.ok()) [[unlikely]] {                                         \
            raise(SIGTRAP);                                                    \
            co_return stat.status();                                           \
        }                                                                      \
    } while (0)

inline auto errcode_to_result(int err) -> Result<folly::Unit>
{
    if (err == 0)
        return folly::Unit();
    return absl::ErrnoToStatus(err, "");
}

inline auto todo(bool should_throw = false,
                 std::source_location sl = std::source_location::current())
{
    fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold,
               "[ERR {}:{} {}] TODO\n", sl.file_name(), sl.line(),
               sl.function_name());
    if (should_throw)
        throw std::runtime_error("TODO stub");
}

inline auto
unimplemented(std::source_location sl = std::source_location::current())
{
    fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold,
               "[ERR {}:{} {}] Unimplemented\n", sl.file_name(), sl.line(),
               sl.function_name());
    throw std::runtime_error("TODO stub");
}

inline static size_t parse_size_str(str i)
{
    size_t processed;
    auto val = std::stoull(i, &processed);
    if (processed == i.size())
        return val;
    else if (processed == 0)
        throw std::invalid_argument("Not a positive number: " + i);

    auto rest = i.substr(processed);
    auto postfix = rest.size() == 1 ? rest[0] : 0;
    if (std::toupper(postfix) == 'G')
        val *= (1024 * 1024 * 1024);
    else if (std::toupper(postfix) == 'M')
        val *= (1024 * 1024);
    else if (std::toupper(postfix) == 'K')
        val *= 1024;
    else
        throw std::invalid_argument("Invalid postfix (not k/m/g): " + rest);

    return val;
}
