#pragma once
#include <cassert>
#include <chrono>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <iostream>
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

#define trace(MSG, ...)                                                        \
    do {                                                                       \
        if (LOGLV <= 0)                                                        \
            fmt::print(stderr, "[TRC {}:{} {}] " MSG "\n", __FILE__, __LINE__, \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define debug(MSG, ...)                                                        \
    do {                                                                       \
        if (LOGLV <= 1)                                                        \
            fmt::print(stderr, "[DBG {}:{} {}] " MSG "\n", __FILE__, __LINE__, \
                       __func__, ##__VA_ARGS__);                               \
    } while (0)

#define log_info(MSG, ...)                                                     \
    do {                                                                       \
        if (LOGLV <= 2)                                                        \
            fmt::print(stderr, fg(fmt::color::yellow) | fmt::emphasis::bold,   \
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
