// SPDX-License-Identifier: GPL-2.0-or-later
// Port shim: replaces shadPS4's spdlog-based logger. Keeps the LOG_* call shape
// (category constant, fmt-style message) so vendored call sites are unchanged.

#pragma once

#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "common/logging/classes.h"

namespace Common::Log {

// The first kEagerLogs occurrences of each message print; after that only every
// kLogEvery-th one does. Recompiler warnings can repeat per instruction, and unbounded output
// would bury the rest of the log.
inline constexpr unsigned long long kEagerLogs = 16;
inline constexpr unsigned long long kLogEvery = 4096;

template <typename... Args>
void Print(std::string_view level, std::string_view cls, fmt::format_string<Args...> format,
           Args&&... args) {
    // fmt's string view type is not std::string_view; key on the literal's text instead.
    const auto text = format.get();
    const std::string_view key{text.data(), text.size()};
    static std::mutex s_mutex;
    static std::unordered_map<std::string_view, unsigned long long> s_counts;
    {
        std::lock_guard lock(s_mutex);
        const unsigned long long count = ++s_counts[key];
        if (count > kEagerLogs && count % kLogEvery != 0) {
            return;
        }
    }
    const std::string message = fmt::format(format, std::forward<Args>(args)...);
    std::fprintf(stderr, "[%.*s] [%.*s] %s\n", static_cast<int>(level.size()), level.data(),
                 static_cast<int>(cls.size()), cls.data(), message.c_str());
}

} // namespace Common::Log

#define LOG_CRITICAL(cls, ...) ::Common::Log::Print("CRITICAL", Common::Log::Class::cls, __VA_ARGS__)
#define LOG_ERROR(cls, ...) ::Common::Log::Print("ERROR", Common::Log::Class::cls, __VA_ARGS__)
#define LOG_WARNING(cls, ...) ::Common::Log::Print("WARN", Common::Log::Class::cls, __VA_ARGS__)
#define LOG_INFO(cls, ...) ::Common::Log::Print("INFO", Common::Log::Class::cls, __VA_ARGS__)
#define LOG_DEBUG(cls, ...) ::Common::Log::Print("DEBUG", Common::Log::Class::cls, __VA_ARGS__)
#define LOG_TRACE(cls, ...) ::Common::Log::Print("TRACE", Common::Log::Class::cls, __VA_ARGS__)
