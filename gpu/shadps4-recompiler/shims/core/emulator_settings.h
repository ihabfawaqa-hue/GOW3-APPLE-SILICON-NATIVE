// SPDX-License-Identifier: GPL-2.0-or-later
// Port shim: fixed recompiler settings for the Metal port.
//  - Shader dumps are off unless GOW3_DUMP_SHADERS is set (debugging aid).
//  - Direct memory access is off, so ReadConst uses the flattened user-data
//    buffer the host builds, not raw guest pointers (no BDA or page table).

#pragma once

#include <cstdlib>

struct EmulatorSettingsStub {
    bool IsDumpShaders() const {
        return std::getenv("GOW3_DUMP_SHADERS") != nullptr;
    }
    bool IsDirectMemoryAccessEnabled() const {
        return false;
    }
};

inline constexpr EmulatorSettingsStub EmulatorSettings{};
