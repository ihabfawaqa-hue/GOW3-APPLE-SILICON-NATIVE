// SPDX-License-Identifier: GPL-2.0-or-later
// Port shim: resolves a per-user directory for optional shader dumps.

#pragma once

#include <cstdlib>
#include <filesystem>

namespace Common::FS {

enum class PathType { ShaderDir };

inline std::filesystem::path GetUserPath(PathType) {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / "Library/Caches/gow3-arm64/shader";
}

} // namespace Common::FS
