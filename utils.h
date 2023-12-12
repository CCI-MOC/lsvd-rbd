#pragma once
#include <cassert>
#include <chrono>
#include <errno.h>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <iostream>
#include <signal.h>
#include <source_location>
#include <span>
#include <sstream>
#include <stdio.h>
#include <string>
#include <vector>

#ifndef LOGLV
#define LOGLV 1
#endif

template <typename T> using sptr = std::shared_ptr<T>;
template <typename T> using uptr = std::unique_ptr<T>;

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

#define log_error(MSG, ...)                                                    \
    do {                                                                       \
        fmt::print(stderr, fg(fmt::terminal_color::red) | fmt::emphasis::bold, \
                   "[ERR {}:{} {}] " MSG "\n", __FILE__, __LINE__, __func__,   \
                   ##__VA_ARGS__);                                             \
    } while (0)

#define log_warn(MSG, ...)                                                     \
    do {                                                                       \
        if (LOGLV <= 3)                                                        \
            fmt::print(stderr,                                                 \
                       fg(fmt::terminal_color::yellow) | fmt::emphasis::bold,  \
                       "[WARN {}:{} {}] " MSG "\n", __FILE__, __LINE__,        \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define trap_to_debugger()                                                     \
    do {                                                                       \
        raise(SIGTRAP);                                                        \
    } while (0)

/**
 * Check return values of libstdc functions. If it's -1, print the error and
 * throw an exception
 */
#define check_ret_errno(ret, MSG, ...)                                         \
    do {                                                                       \
        if (ret == -1) {                                                       \
            auto en = errno;                                                   \
            auto s = fmt::format("[ERR {}:{} {} | errno {}/{}] " MSG "\n",     \
                                 __FILE__, __LINE__, __func__, en,             \
                                 strerror(en), ##__VA_ARGS__);                 \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold, s);  \
            throw std::runtime_error(s);                                       \
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

inline std::vector<std::string> split_string_on_char(const std::string &s,
                                                     char delim)
{
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;

    while (getline(ss, item, delim)) {
        result.push_back(item);
    }

    return result;
}

inline std::string string_join(const std::vector<std::string> &strings,
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

inline std::chrono::time_point<std::chrono::system_clock> tnow()
{
    return std::chrono::high_resolution_clock::now();
}

constexpr std::chrono::microseconds
tus(std::chrono::time_point<std::chrono::system_clock> start,
    std::chrono::time_point<std::chrono::system_clock> end)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start);
}

constexpr int64_t tdus(std::chrono::time_point<std::chrono::system_clock> start,
                       std::chrono::time_point<std::chrono::system_clock> end)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
        .count();
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
