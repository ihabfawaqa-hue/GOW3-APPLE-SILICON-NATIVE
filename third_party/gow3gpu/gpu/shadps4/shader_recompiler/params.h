// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <span>
#include <utility>
#include <vector>
#include "common/types.h"

namespace Shader {

/**
 * Compilation parameters used to identify and locate a guest shader program.
 */
struct ShaderParams {
    static constexpr u32 NumShaderUserData = 16;

    std::span<const u32, NumShaderUserData> user_data;
    std::span<const u32> code;
    u64 hash;
    VAddr base_address;
    std::shared_ptr<const std::vector<u32>> code_storage;

    ShaderParams(std::span<const u32, NumShaderUserData> user_data_,
                 std::span<const u32> code_, u64 hash_, VAddr base_address_,
                 std::shared_ptr<const std::vector<u32>> code_storage_ = {})
        : user_data{user_data_}, code{code_}, hash{hash_}, base_address{base_address_},
          code_storage{std::move(code_storage_)} {}

    VAddr Base() const noexcept {
        return base_address;
    }
};

} // namespace Shader
