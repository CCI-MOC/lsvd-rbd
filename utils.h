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
#define LOGLV 0
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
            fmt::print(stderr, fg(fmt::color::dark_violet),                    \
                       "[DBG {}:{} {}] " MSG "\n", __FILE__, __LINE__,         \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define log_info(MSG, ...)                                                     \
    do {                                                                       \
        if (LOGLV <= 2)                                                        \
            fmt::print(stderr,                                                 \
                       fg(fmt::color::dark_cyan) | fmt::emphasis::bold,        \
                       "[INFO {}:{} {}] " MSG "\n", __FILE__, __LINE__,        \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define log_error(MSG, ...)                                                    \
    do {                                                                       \
        if (LOGLV <= 3)                                                        \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold,      \
                       "[ERR {}:{} {}] " MSG "\n", __FILE__, __LINE__,         \
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
#define check_ret(ret, MSG, ...)                                               \
    do {                                                                       \
        if (ret == -1) {                                                       \
            auto en = errno;                                                   \
            auto s = fmt::format("[ERR {}:{} {} | errno {}]: {}; " MSG,        \
                                 __FILE__, __LINE__, __func__, en,             \
                                 strerror(en), ##__VA_ARGS__);                 \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold, s);  \
            throw std::runtime_error(MSG);                                     \
        }                                                                      \
    } while (0)

#define UNIMPLEMENTED()                                                        \
    do {                                                                       \
        fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold,          \
                   "[ERR {}:{} {}] Unimplemented\n", __FILE__, __LINE__,       \
                   __func__);                                                  \
        throw std::runtime_error("Unimplemented");                             \
    } while (0)

#ifndef NDEBUG
#define ASSERT(condition, message)                                             \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold,      \
                       "[ERR {}:{} {}] Assertion `" #condition                 \
                       "` failed: {}\n",                                       \
                       __FILE__, __LINE__, __func__, message);                 \
            throw std::runtime_error("Assertion failed");                      \
        }                                                                      \
    } while (false)
#else
#define ASSERT(condition, message)                                             \
    do {                                                                       \
    } while (false)
#endif

#ifndef NDEBUG
#define ASSERT_VAL(val, expected, message)                                     \
    do {                                                                       \
        if (val != expected) {                                                 \
            fmt::print(stderr, fg(fmt::color::red) | fmt::emphasis::bold,      \
                       "[ERR {}:{} {}] Assertion `" #val " == " #expected      \
                       "` failed, expected {}, found {}: {}\n",                \
                       __FILE__, __LINE__, __func__, expected, val, message);  \
            throw std::runtime_error("Assertion failed");                      \
        }                                                                      \
    } while (false)
#else
#define ASSERT_VAL(condition, message)                                         \
    do {                                                                       \
    } while (false)
#endif

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
