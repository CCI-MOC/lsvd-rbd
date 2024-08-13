#pragma once

// #include "folly/FBVector.h"
#include <boost/outcome.hpp>
#include <boost/stacktrace.hpp>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <linux/fs.h>
#include <optional>
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

namespace outcome = boost::outcome_v2;
template <typename T> using result = outcome::result<T>;

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

#define PASSTHRU_FAILURE(res)                                                  \
    do {                                                                       \
        if (res.has_error()) {                                                 \
            return res;                                                        \
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

#define FAILURE_IF_NEGATIVE(rc)                                                \
    do {                                                                       \
        if (rc < 0) {                                                          \
            return outcome::failure(                                           \
                std::error_code(-rc, std::generic_category()));                \
        }                                                                      \
    } while (0)

/**
 * Check return values of functions that return -errno in the case of an error
 * and convert it to a outcome::failure() instead on failure
 */
#define PR_FAILURE_IF_NEGATIVE(ret, MSG, ...)                                  \
    do {                                                                       \
        if (ret < 0) {                                                         \
            auto en = -ret;                                                    \
            auto ec = std::error_code(en, std::generic_category());            \
            auto fs = fmt::format(MSG "\n", ##__VA_ARGS__);                    \
            auto s = fmt::format("[ERR {}:{} {} | errno {}/{}] {}", __FILE__,  \
                                 __LINE__, __func__, en, ec.message(), fs);    \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold, s);  \
            return outcome::failure(ec);                                       \
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

template <typename T> inline auto errcode_to_result(int rc) -> result<T>
{
    if (rc > 0)
        return outcome::failure(std::error_code(rc, std::generic_category()));
    return outcome::success();
}

enum class LsvdError {
    Success,
    InvalidMagic,
    InvalidVersion,
    InvalidObjectType,
    MissingData,
};

namespace std
{
template <> struct is_error_code_enum<LsvdError> : true_type {
};
}; // namespace std

namespace detail
{
// Define a custom error code category derived from std::error_category
class BackendError_category : public std::error_category
{
  public:
    // Return a short descriptive name for the category
    virtual const char *name() const noexcept override final
    {
        return "Data validity error";
    }

    // Return what each enum means in text
    virtual std::string message(int c) const override final
    {
        switch (static_cast<LsvdError>(c)) {
        case LsvdError::Success:
            return "passed validity checks";
        case LsvdError::InvalidMagic:
            return "invalid magic number in the header";
        case LsvdError::InvalidVersion:
            return "version number unsupported";
        case LsvdError::InvalidObjectType:
            return "does not match required object type";
        case LsvdError::MissingData:
            return "required data not found";
        }
    }
};
} // namespace detail

extern inline const detail::BackendError_category &BackendError_category()
{
    static detail::BackendError_category c;
    return c;
}

inline std::error_code make_error_code(LsvdError e)
{
    static detail::BackendError_category c;
    return {static_cast<int>(e), c};
}

inline int result_to_rc(result<void> res)
{
    if (res.has_value())
        return 0;
    else
        return res.error().value();
}

inline int result_to_rc(result<int> res)
{
    if (res.has_value())
        return res.value();
    else
        return res.error().value();
}
