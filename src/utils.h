#pragma once
#include <boost/outcome.hpp>
#include <boost/stacktrace.hpp>
#include <fmt/color.h>
#include <folly/experimental/coro/Promise.h>
#include <folly/experimental/coro/Task.h>
#include <folly/logging/xlog.h>
#include <future>
#include <linux/fs.h>
#include <optional>
#include <source_location>
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
template <typename T> using fvec = folly::fbvector<T>;

using str = std::string;

#define LOGERR_AND_RET_IF_FAIL(res, msg, ...)                                  \
    do {                                                                       \
        if (res.has_failure()) {                                               \
            auto outmsg = fmt::format(msg, __VA_ARGS__);                       \
            XLOGF(ERR, "{}: {}", outmsg, res.error().message());               \
            return res.error();                                                \
        }                                                                      \
    } while (0)

template <typename T> inline auto errcode_to_result(int err) -> Result<T>
{
    if (err == 0)
        return outcome::success();
    return outcome::failure(std::error_code(err, std::system_category()));
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
