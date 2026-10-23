// SPDX-License-Identifier: GPL-2.0-or-later
// Port shim: replaces shadPS4's common/assert.h, which is coupled to its logger.
// Failures print to stderr and abort so a bad translation is never silently ignored.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"

namespace Common::Assert {

// Set while the caller can recover from a failed assertion (the translator, through
// ThrowOnFailure). Failures then throw std::runtime_error instead of aborting the process.
inline thread_local bool g_throw_on_failure = false;

struct ThrowOnFailure {
    ThrowOnFailure() {
        g_throw_on_failure = true;
    }
    ~ThrowOnFailure() {
        g_throw_on_failure = false;
    }
};

[[noreturn]] inline void Fail(const char* condition, const char* file, int line,
                              const std::string& message) {
    if (g_throw_on_failure) {
        throw std::runtime_error(std::string("assertion failed: ") + condition + " at " + file +
                                 ":" + std::to_string(line) + ": " + message);
    }
    std::fprintf(stderr, "[shader-recompiler] assertion failed: %s at %s:%d: %s\n", condition,
                 file, line, message.c_str());
    std::abort();
}

// Message formatting that also accepts no message at all, as upstream ASSERT_MSG call sites do.
inline std::string Format() {
    return {};
}

template <typename... Args>
std::string Format(fmt::format_string<Args...> format, Args&&... args) {
    return fmt::format(format, std::forward<Args>(args)...);
}

} // namespace Common::Assert

#define ASSERT(cond)                                                                             \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            ::Common::Assert::Fail(#cond, __FILE__, __LINE__, "");                               \
        }                                                                                        \
    } while (0)

#define ASSERT_MSG(cond, ...)                                                                    \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            ::Common::Assert::Fail(#cond, __FILE__, __LINE__, ::Common::Assert::Format(__VA_ARGS__)); \
        }                                                                                        \
    } while (0)

#define DEBUG_ASSERT(cond) ASSERT(cond)
#define DEBUG_ASSERT_MSG(cond, ...) ASSERT_MSG(cond, __VA_ARGS__)

#define UNREACHABLE() ::Common::Assert::Fail("unreachable", __FILE__, __LINE__, "")
#define UNREACHABLE_MSG(...)                                                                     \
    ::Common::Assert::Fail("unreachable", __FILE__, __LINE__, ::Common::Assert::Format(__VA_ARGS__))

#define UNIMPLEMENTED() ::Common::Assert::Fail("unimplemented", __FILE__, __LINE__, "")
#define UNIMPLEMENTED_MSG(...)                                                                   \
    ::Common::Assert::Fail("unimplemented", __FILE__, __LINE__, ::Common::Assert::Format(__VA_ARGS__))
