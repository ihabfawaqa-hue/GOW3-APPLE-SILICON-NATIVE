// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/bit_array.h"
#include "common/types.h"

namespace VideoCore {

// The tracker protects host pages with mprotect on Apple Silicon. macOS uses
// 16 KiB VM pages on arm64, so its tracker granularity must match the host or
// each protection change is invalid and adjacent 4 KiB states cannot be
// represented independently by the OS page tables.
#if defined(__APPLE__) && defined(__aarch64__)
constexpr u64 TRACKER_PAGE_BITS = 14; // 16K host pages
#else
constexpr u64 TRACKER_PAGE_BITS = 12; // 4K pages
#endif
constexpr u64 TRACKER_BYTES_PER_PAGE = 1ULL << TRACKER_PAGE_BITS;

constexpr u64 TRACKER_HIGHER_PAGE_BITS = 22; // each region is 4MB
constexpr u64 TRACKER_HIGHER_PAGE_SIZE = 1ULL << TRACKER_HIGHER_PAGE_BITS;
constexpr u64 TRACKER_HIGHER_PAGE_MASK = TRACKER_HIGHER_PAGE_SIZE - 1ULL;
constexpr u64 NUM_PAGES_PER_REGION = TRACKER_HIGHER_PAGE_SIZE / TRACKER_BYTES_PER_PAGE;

enum class Type {
    CPU,
    GPU,
};

using RegionBits = Common::BitArray<NUM_PAGES_PER_REGION>;

} // namespace VideoCore
