#pragma once

#include <cstdint>
#include <cstdlib>
#include <fmt/core.h>
#include <string>

namespace libusb::proxy::log {

namespace {

inline int log_level() {
    static int log_level = [](){
        auto var = std::getenv("LIBUSB_PROXY_DEBUG");
        if (!var) return 0;
        try {
            return std::stoi(std::string(var));
        } catch (...) {
            return 0;
        }
    }();

    return log_level;
}

}

template <typename... Args>
void err(fmt::format_string<Args...>&& fmt_str, Args&&... args) {
    if (log_level() < 1) return;
    fmt::print(stderr, "error: {}\n", fmt::format(
        std::forward<fmt::format_string<Args...>>(fmt_str), std::forward<Args>(args)...));
}

template <typename... Args>
void warn(fmt::format_string<Args...>&& fmt_str, Args&&... args) {
    if (log_level() < 2) return;
    fmt::print(stderr, "warn: {}\n", fmt::format(
        std::forward<fmt::format_string<Args...>>(fmt_str), std::forward<Args>(args)...));
}

template <typename... Args>
void info(fmt::format_string<Args...>&& fmt_str, Args&&... args) {
    if (log_level() < 3) return;
    fmt::print(stderr, "info: {}\n", fmt::format(
        std::forward<fmt::format_string<Args...>>(fmt_str), std::forward<Args>(args)...));
}

template <typename... Args>
void dbg(fmt::format_string<Args...>&& fmt_str, Args&&... args) {
    if (log_level() < 4) return;
    fmt::print(stderr, "debug: {}\n", fmt::format(
        std::forward<fmt::format_string<Args...>>(fmt_str), std::forward<Args>(args)...));
}

}