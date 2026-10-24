// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "gow3_toggles.h"
#include "common/assert.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"

namespace Shader::Gcn {

namespace {
thread_local const FetchShaderSnapshot* compiler_snapshot = nullptr;
}

AmdGpu::Buffer VertexAttribute::GetSharp(const Shader::Info& info) const noexcept {
    if (compiler_snapshot && compiler_snapshot->info == &info && compiler_snapshot->fetch) {
        const auto& attributes = compiler_snapshot->fetch->attributes;
        for (size_t i = 0; i < attributes.size(); ++i) {
            if (attributes[i] == *this) {
                return compiler_snapshot->buffers[i];
            }
        }
        UNREACHABLE_MSG("Fetch attribute missing from compiler snapshot");
    }
    auto buffer = info.ReadUdReg<AmdGpu::Buffer>(sgpr_base, dword_offset);
    buffer.base_address += inst_offset;
    if (data_format) {
        buffer.data_format = data_format;
        buffer.num_format = num_format;
    }
    return buffer;
}

void FetchShaderSnapshot::Capture(const Info& source) {
    info = &source;
    fetch = ParseFetchShader(source);
    buffers.clear();
    if (fetch) {
        for (const auto& attribute : fetch->attributes) {
            buffers.push_back(attribute.GetSharp(source));
        }
    }
}

ScopedFetchShaderSnapshot::ScopedFetchShaderSnapshot(const FetchShaderSnapshot& snapshot)
    : previous{compiler_snapshot} {
    compiler_snapshot = &snapshot;
}

ScopedFetchShaderSnapshot::~ScopedFetchShaderSnapshot() {
    compiler_snapshot = previous;
}

/**
 * s_load_dwordx4 s[8:11], s[2:3], 0x00
 * s_load_dwordx4 s[12:15], s[2:3], 0x04
 * s_load_dwordx4 s[16:19], s[2:3], 0x08
 * s_waitcnt     lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen
 * buffer_load_format_xyz v[8:10], v0, s[12:15], 0 idxen
 * buffer_load_format_xy v[12:13], v0, s[16:19], 0 idxen
 * s_waitcnt     0
 * s_setpc_b64   s[0:1]

 * s_load_dwordx4  s[4:7], s[2:3], 0x0
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[4:7], 0 idxen
 * s_load_dwordx4  s[4:7], s[2:3], 0x8
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[8:11], v0, s[4:7], 0 idxen
 * s_waitcnt       vmcnt(0) & expcnt(0) & lgkmcnt(0)
 * s_setpc_b64     s[0:1]

 * A normal fetch shader looks like the above, the instructions are generated
 * using input semantics on cpu side. Load instructions can either be separate or interleaved
 * We take the reverse way, extract the original input semantics from these instructions.
 **/

static bool IsTypedBufferLoad(const Gcn::GcnInst& inst) {
    return inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_X ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XY ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XYZ ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XYZW;
}

bool CopyFetchShaderCode(const Info& info, u32 sgpr_base, std::vector<u32>& code,
                         VAddr* guest_address, size_t* valid_bytes) {
    constexpr u64 max_fetch_shader_bytes = 16 * 1024;
    const auto user_data = info.UserData();
    if (sgpr_base > user_data.size() || user_data.size() - sgpr_base < 2) {
        return false;
    }

    VAddr address{};
    std::memcpy(&address, user_data.data() + sgpr_base, sizeof(address));
    address &= 0xFFFFFFFFFFFFULL;
    const u64 available = Core::Memory::Instance()->ClampRangeSize(address,
                                                                    max_fetch_shader_bytes);
    const u64 bytes = std::min(available, max_fetch_shader_bytes) & ~u64{3};
    if (bytes < sizeof(u32)) {
        return false;
    }

    // Some GCN encodings read one or two trailing dwords. Keep zeroed guard words beyond
    // the guest range while the decoder is bounded by valid_bytes.
    code.assign(bytes / sizeof(u32) + 4, 0);
    if (!Core::Memory::Instance()->CopySparseMemory(
            address, reinterpret_cast<u8*>(code.data()), bytes)) {
        code.clear();
        return false;
    }
    if (guest_address) {
        *guest_address = address;
    }
    if (valid_bytes) {
        *valid_bytes = static_cast<size_t>(bytes);
    }
    return true;
}

std::optional<FetchShaderData> ParseFetchShader(const Shader::Info& info) {
    if (compiler_snapshot && compiler_snapshot->info == &info) {
        return compiler_snapshot->fetch;
    }
    if (!info.has_fetch_shader) {
        return std::nullopt;
    }

    std::vector<u32> code_storage;
    VAddr code_address{};
    size_t code_bytes{};
    if (!CopyFetchShaderCode(info, info.fetch_shader_sgpr_base, code_storage, &code_address,
                             &code_bytes)) {
        LOG_ERROR(Render_Recompiler, "Fetch shader guest range is unavailable for shader {:#018x}",
                  info.pgm_hash);
        return std::nullopt;
    }
    const auto* code = code_storage.data();

    // gow3: the pipeline cache parses the fetch shader on every draw. Results are cached
    // per thread by code address and revalidated against a copy of the code, which is
    // much cheaper than decoding it again.
    struct CachedParse {
        std::vector<u32> code;
        FetchShaderData data;
    };
    thread_local std::unordered_map<VAddr, CachedParse> cache;
    if (const auto it = cache.find(code_address);
        !Gow3Toggle::Disabled(Gow3Toggle::FetchShaderCache) && it != cache.end() &&
        it->second.code.size() <= code_storage.size() &&
        std::memcmp(code, it->second.code.data(), it->second.code.size() * sizeof(u32)) == 0) {
        return it->second.data;
    }

    FetchShaderData data{};
    GcnCodeSlice code_slice(code, code + code_bytes / sizeof(u32));
    GcnDecodeContext decoder;

    struct VsharpLoad {
        u32 dword_offset{};
        u32 base_sgpr{};
    };
    std::array<VsharpLoad, 104> loads{};

    u32 semantic_index = 0;
    bool terminated = false;
    while (!code_slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(code_slice);
        data.size += inst.length;

        if (inst.opcode == Opcode::S_SETPC_B64) {
            terminated = true;
            break;
        }

        if (inst.inst_class == InstClass::ScalarMemRd) {
            loads[inst.dst[0].code] = VsharpLoad{inst.control.smrd.offset, inst.src[0].code * 2};
            continue;
        }

        if (inst.opcode == Opcode::V_ADD_I32) {
            const auto vgpr = inst.dst[0].code;
            const auto sgpr = s8(inst.src[0].code);
            switch (vgpr) {
            case 0: // V0 is always the vertex offset
                data.vertex_offset_sgpr = sgpr;
                break;
            case 3: // V3 is always the instance offset
                data.instance_offset_sgpr = sgpr;
                break;
            default:
                UNREACHABLE();
            }
        }

        if (inst.inst_class == InstClass::VectorMemBufFmt) {
            // SRSRC is in units of 4 SPGRs while SBASE is in pairs of SGPRs
            const u32 base_sgpr = inst.src[2].code * 4;
            auto& attrib = data.attributes.emplace_back();
            attrib.semantic = semantic_index++;
            attrib.dest_vgpr = inst.src[1].code;
            attrib.num_elements = inst.control.mubuf.count;
            attrib.sgpr_base = loads[base_sgpr].base_sgpr;
            attrib.dword_offset = loads[base_sgpr].dword_offset;
            attrib.inst_offset = inst.control.mtbuf.offset;
            attrib.instance_data = inst.src[0].code;
            if (IsTypedBufferLoad(inst)) {
                attrib.data_format = inst.control.mtbuf.dfmt;
                attrib.num_format = inst.control.mtbuf.nfmt;
            }
        }
    }

    if (!terminated) {
        LOG_ERROR(Render_Recompiler,
                  "Fetch shader at guest address {:#x} has no S_SETPC_B64 within {} bytes",
                  code_address, code_storage.size() * sizeof(u32));
        return std::nullopt;
    }

    if (cache.size() >= 4096) {
        cache.clear();
    }
    code_storage.resize(data.size / sizeof(u32) + 4, 0);
    cache[code_address] = CachedParse{std::move(code_storage), data};
    return data;
}

} // namespace Shader::Gcn
