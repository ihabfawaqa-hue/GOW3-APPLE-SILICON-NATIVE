// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>
#include "common/assert.h"
#include "common/types.h"
#include "gow3_toggles.h"
#include "core/memory.h"
#include "shader_recompiler/params.h"

namespace AmdGpu {

static constexpr u32 NUM_USER_DATA = 16;

using UserData = std::array<u32, NUM_USER_DATA>;

struct BinaryInfo {
    static constexpr std::array<u8, 7> signature_ref = {0x4f, 0x72, 0x62, 0x53,
                                                        0x68, 0x64, 0x72}; // OrbShdr

    std::array<u8, sizeof(signature_ref)> signature;
    u8 version;
    u32 pssl_or_cg : 1;
    u32 cached : 1;
    u32 type : 4;
    u32 source_type : 2;
    u32 length : 24;
    u8 chunk_usage_base_offset_in_dw;
    u8 num_input_usage_slots;
    u8 is_srt : 1;
    u8 is_srt_used_info_valid : 1;
    u8 is_extended_usage_info : 1;
    u8 reserved2 : 5;
    u8 reserved3;
    u64 shader_hash;
    u32 crc32;

    bool Valid() const {
        return signature == signature_ref;
    }
};

enum class FpRoundMode : u32 {
    NearestEven = 0,
    PlusInf = 1,
    MinInf = 2,
    ToZero = 3,
};

enum class FpDenormMode : u32 {
    InOutFlush = 0,
    InAllowOutFlush = 1,
    InFlushOutAllow = 2,
    InOutAllow = 3,
};

struct ShaderProgram {
    u64 address : 40;
    struct {
        u32 num_vgprs : 6;
        u32 num_sgprs : 4;
        u32 priority : 2;
        FpRoundMode fp_round_mode32 : 2;
        FpRoundMode fp_round_mode64 : 2;
        FpDenormMode fp_denorm_mode32 : 2;
        FpDenormMode fp_denorm_mode64 : 2;
        u32 : 4;
        u32 vgpr_comp_cnt : 2;
        u32 : 6;
        u32 scratch_en : 1;
        u32 num_user_regs : 5;
        u32 : 1;
        u32 oc_lds_en : 1;
    } settings;
    UserData user_data;

    template <typename T = u8*>
    const T Address() const {
        return std::bit_cast<T>(address << 8);
    }

    [[nodiscard]] VAddr AddressValue() const noexcept {
        return address << 8;
    }

    [[nodiscard]] u32 NumVgprs() const {
        // Each increment allocates 4 registers, where 0 = 4 registers.
        return (settings.num_vgprs + 1) * 4;
    }
};

struct VsOutputConfig {
    u32 : 1;
    u32 export_count_min_one : 5;
    u32 half_pack : 1;

    u32 NumExports() const {
        return export_count_min_one + 1;
    }
};

struct PsInputControl {
    u32 input_offset : 5;
    u32 use_default : 1;
    u32 : 2;
    u32 default_value : 2;
    u32 flat_shade : 1;
};

struct PsInput {
    u32 persp_sample_ena : 1;
    u32 persp_center_ena : 1;
    u32 persp_centroid_ena : 1;
    u32 persp_pull_model_ena : 1;
    u32 linear_sample_ena : 1;
    u32 linear_center_ena : 1;
    u32 linear_centroid_ena : 1;
    u32 line_stipple_tex_ena : 1;
    u32 pos_x_float_ena : 1;
    u32 pos_y_float_ena : 1;
    u32 pos_z_float_ena : 1;
    u32 pos_w_float_ena : 1;
    u32 front_face_ena : 1;
    u32 ancillary_ena : 1;
    u32 sample_coverage_ena : 1;
    u32 pos_fixed_pt_ena : 1;

    bool operator==(const PsInput&) const = default;
};

struct BarycentricControl {
    u32 : 24;
    u32 front_face_all_bits : 1;
};
static_assert(sizeof(BarycentricControl) == sizeof(u32));

enum class ShaderExportComp : u32 {
    None = 0,
    OneComp = 1,
    TwoComp = 2,
    FourCompCompressed = 3,
    FourComp = 4,
};

struct ShaderPosFormat {
    ShaderExportComp pos0 : 4;
    ShaderExportComp pos1 : 4;
    ShaderExportComp pos2 : 4;
    ShaderExportComp pos3 : 4;
};

enum class ShaderExportFormat : u32 {
    Zero = 0,
    R_32 = 1,
    GR_32 = 2,
    AR_32 = 3,
    ABGR_FP16 = 4,
    ABGR_UNORM16 = 5,
    ABGR_SNORM16 = 6,
    ABGR_UINT16 = 7,
    ABGR_SINT16 = 8,
    ABGR_32 = 9,
};

struct ColorExportFormat {
    u32 raw;

    [[nodiscard]] ShaderExportFormat GetFormat(const u32 buf_idx) const {
        return static_cast<ShaderExportFormat>((raw >> (buf_idx * 4)) & 0xfu);
    }
};

struct ComputeProgram {
    u32 dispatch_initiator;
    u32 dim_x;
    u32 dim_y;
    u32 dim_z;
    u32 start_x;
    u32 start_y;
    u32 start_z;
    struct {
        u16 full;
        u16 partial;
    } num_thread_x, num_thread_y, num_thread_z;
    u32 pad0;
    u32 max_wave_id : 12;
    u64 address : 40;
    std::array<u32, 4> pad1;
    struct {
        u32 num_vgprs : 6;
        u32 num_sgprs : 4;
        u32 : 2;
        FpRoundMode fp_round_mode32 : 2;
        FpRoundMode fp_round_mode64 : 2;
        FpDenormMode fp_denorm_mode32 : 2;
        FpDenormMode fp_denorm_mode64 : 2;
        u32 : 12;
        u32 scratch_en : 1;
        u32 num_user_regs : 5;
        u32 : 1;
        u32 tgid_enable : 3;
        u32 : 5;
        u32 lds_dwords : 9;
    } settings;
    u32 pad2;
    u32 resource_limits;
    std::array<u32, 42> pad3;
    UserData user_data;

    template <typename T = u8*>
    const T Address() const {
        return std::bit_cast<T>(address << 8);
    }

    [[nodiscard]] VAddr AddressValue() const noexcept {
        return address << 8;
    }

    u32 SharedMemSize() const noexcept {
        // lds_dwords is in units of 128 dwords. We return bytes.
        return settings.lds_dwords * 128 * 4;
    }

    u32 NumWorkgroups() const noexcept {
        return dim_x * dim_y * dim_z;
    }

    bool IsTgidEnabled(u32 i) const noexcept {
        return (settings.tgid_enable >> i) & 1;
    }
};

static std::optional<BinaryInfo> SearchBinaryInfo(std::span<const u32> code) {
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    const auto read_info = [&](std::size_t dword_offset) -> std::optional<BinaryInfo> {
        const std::size_t byte_offset = dword_offset * sizeof(u32);
        if (byte_offset > code.size_bytes() ||
            sizeof(BinaryInfo) > code.size_bytes() - byte_offset) {
            return std::nullopt;
        }
        BinaryInfo info{};
        std::memcpy(&info, reinterpret_cast<const u8*>(code.data()) + byte_offset,
                    sizeof(info));
        return info.Valid() ? std::optional{info} : std::nullopt;
    };
    if (code.size() >= 2 && code[0] == token_mov_vcchi) {
        if (auto info = read_info((code[1] + 1) * 2)) {
            return info;
        }
    }
    constexpr u32 search_limit = 0x4000;
    const std::size_t count = std::min<std::size_t>(code.size(), search_limit);
    for (std::size_t offset = 0; offset < count; ++offset) {
        if (auto info = read_info(offset)) {
            return info;
        }
    }
    return std::nullopt;
}

static Shader::ShaderParams GetParams(const auto& sh) {
    constexpr std::size_t max_shader_words = 0x4000;
    const VAddr base = sh.AddressValue();
    auto* memory = Core::Memory::Instance();
    // gow3: every stage of every draw comes here. Copying (and zero-filling) 64 KB to find the
    // binary info took ~40% of the GPU command thread in gameplay. Each thread keeps the code
    // it found per shader address and reuses it while the guest bytes up to the end of the
    // binary info are unchanged: a copy of a few KB instead. GOW3_SHADER_CODE_CACHE=0 turns
    // it off.
    static const bool use_cache = [] {
        const char* value = std::getenv("GOW3_SHADER_CODE_CACHE");
        return !(value && value[0] == '0');
    }();
    // The guest does not rewrite the code a submission draws with before it ends, so an
    // entry checked during the current submission is reused without the copy (most stages
    // repeat across a submission's draws).
    struct Cached {
        std::vector<u32> bytes; // guest words from the base through the binary info
        u64 hash;
        std::shared_ptr<std::vector<u32>> code;
        u64 checked_epoch = ~0ull;
    };
    thread_local std::unordered_map<VAddr, Cached> cache;
    thread_local std::vector<u32> scratch;
    if (use_cache) {
        if (const auto it = cache.find(base); it != cache.end()) {
            auto& entry = it->second;
            const u64 epoch = Gow3Stats::submission_epoch.load(std::memory_order_acquire);
            if (entry.checked_epoch == epoch &&
                !Gow3Toggle::Disabled(Gow3Toggle::ShaderCodeOncePerSubmission)) {
                return Shader::ShaderParams{sh.user_data, std::span<const u32>{*entry.code},
                                            entry.hash, base, entry.code};
            }
            scratch.resize(entry.bytes.size());
            if (memory->CopySparseMemory(base, reinterpret_cast<u8*>(scratch.data()),
                                         scratch.size() * sizeof(u32)) &&
                std::memcmp(scratch.data(), entry.bytes.data(),
                            scratch.size() * sizeof(u32)) == 0) {
                entry.checked_epoch = epoch;
                return Shader::ShaderParams{sh.user_data, std::span<const u32>{*entry.code},
                                            entry.hash, base, entry.code};
            }
            cache.erase(it);
        }
    }
    auto code = std::make_shared<std::vector<u32>>(max_shader_words);
    if (!memory->CopySparseMemory(base, reinterpret_cast<u8*>(code->data()),
                                  code->size() * sizeof(u32))) {
        return {sh.user_data, {}, 0, base};
    }
    const auto bininfo = SearchBinaryInfo(*code);
    if (!bininfo || bininfo->length == 0 || bininfo->length % sizeof(u32) != 0 ||
        bininfo->length > code->size() * sizeof(u32)) {
        return {sh.user_data, {}, 0, base};
    }
    if (use_cache) {
        // SearchBinaryInfo returns a copy: find where it was read from.
        const auto* bytes = reinterpret_cast<const u8*>(code->data());
        const u64 total = code->size() * sizeof(u32);
        for (u64 offset = 0; offset + sizeof(BinaryInfo) <= total; offset += sizeof(u32)) {
            if (std::memcmp(bytes + offset, &*bininfo, sizeof(BinaryInfo)) != 0) {
                continue;
            }
            const u64 words = std::max<u64>(bininfo->length,
                                            offset + sizeof(BinaryInfo) + sizeof(u32) - 1) /
                              sizeof(u32);
            auto stored = std::make_shared<std::vector<u32>>(
                code->begin(), code->begin() + bininfo->length / sizeof(u32));
            if (cache.size() >= 8192) {
                cache.clear(); // addresses of shaders the game has since freed
            }
            cache[base] = Cached{std::vector<u32>(code->begin(), code->begin() + words),
                                 bininfo->shader_hash, stored,
                                 Gow3Stats::submission_epoch.load(std::memory_order_acquire)};
            break;
        }
    }
    code->resize(bininfo->length / sizeof(u32));
    const std::span<const u32> shader_code{*code};
    return Shader::ShaderParams{sh.user_data, shader_code, bininfo->shader_hash, base, code};
}

} // namespace AmdGpu
