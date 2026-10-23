// SPDX-License-Identifier: GPL-2.0-or-later
// Port probe (not upstream): translates the captured GoW3 draw's vertex and pixel shaders
// through the vendored recompiler, then SPIR-V, then SPIRV-Cross MSL.
//
// usage: recomp_probe <capture-dir> <out-dir>
//   <capture-dir> is a gpu-captures directory from a bounded run (draw_largest_*.bin).
//   <out-dir> receives vs.spv / vs.metal / ps.spv / ps.metal.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "port/guest_memory.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

#include "spirv_msl.hpp"

namespace {

// Guest memory captured with the draw (draw_largest.json). Addresses are the guest VAs the
// shaders use; each region is the captured byte window at that address.
struct Region {
    u64 base;
    std::vector<u8> bytes;
};
std::vector<Region> g_regions;

std::vector<u8> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[probe] cannot read %s\n", path.c_str());
        std::exit(1);
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
}

void WriteFile(const std::string& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

// Host hook used by the recompiler (see port/guest_memory.h).
} // namespace

namespace Shader {

const void* GuestToHost(u64 guest_addr) {
    for (const auto& region : g_regions) {
        if (guest_addr >= region.base && guest_addr < region.base + region.bytes.size()) {
            return region.bytes.data() + (guest_addr - region.base);
        }
    }
    std::fprintf(stderr, "[probe] unmapped guest address 0x%llx\n",
                 static_cast<unsigned long long>(guest_addr));
    std::abort();
}

} // namespace Shader

namespace {

// user_data[16] for each stage, copied from draw_largest.json in the capture.
constexpr std::array<u32, 16> kVsUserData{
    4217559780u, 0u, 257965188u, 1u, 0u, 0u, 0u, 0u, 126703936u, 4194304u, 254u, 537031172u,
    257965044u, 1048577u, 9u, 537362348u};
constexpr std::array<u32, 16> kPsUserData{
    0u, 0u, 224396332u, 1u, 257949720u, 1048577u, 1u, 537362348u,
    15720448u, 0u, 0u, 0u, 146u, 0u, 16777216u, 0u};

// Guest addresses from draw_largest.json.
constexpr u64 kVsCodeAddr = 0xfb62d800ULL;
constexpr u64 kPsCodeAddr = 0xfb62d400ULL;
constexpr u64 kFetchAddr = 0xfb62dae4ULL;
constexpr u64 kDescriptorTableAddr = 0x10f603c84ULL;

void Translate(const char* name, Shader::HwStage hw_stage, Shader::SwStage sw_stage,
               const std::vector<u8>& code_bytes, const std::array<u32, 16>& user_data,
               const std::string& out_dir) {
    const std::span<const u32> code{reinterpret_cast<const u32*>(code_bytes.data()),
                                    code_bytes.size() / sizeof(u32)};
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, 16>{user_data},
        .code = code,
        .hash = 0x5eed0000ULL,
    };

    Shader::Info info{hw_stage, sw_stage, params};
    Shader::RuntimeInfo runtime_info{};
    runtime_info.sw_stage = sw_stage;
    runtime_info.hw_stage = hw_stage;
    runtime_info.props.num_user_data = 16;
    if (hw_stage == Shader::HwStage::Fragment) {
        // Color target 0 as a plain R8G8B8A8 unorm surface with identity swizzle. Without an
        // export format the translator drops the export, so the MSL body would be empty.
        auto& mrt0 = runtime_info.hw.fs.color_buffers[0];
        mrt0.data_format = AmdGpu::DataFormat::Format8_8_8_8;
        mrt0.num_format = AmdGpu::NumberFormat::Unorm;
        mrt0.export_format = AmdGpu::ShaderExportFormat::ABGR_FP16;
        mrt0.swizzle.array = {AmdGpu::CompSwizzle::Red, AmdGpu::CompSwizzle::Green,
                              AmdGpu::CompSwizzle::Blue, AmdGpu::CompSwizzle::Alpha};
    }

    Shader::Profile profile{};
    profile.supported_spirv = 0x00010000;
    profile.subgroup_size = 32;

    Shader::Pools pools;
    Shader::IR::Program program =
        Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    std::printf("[probe] %s: translated %zu dwords, %zu blocks, buffers=%zu images=%zu "
                "samplers=%zu uses_dma=%d readconst=0x%x fetch=%d\n",
                name, code.size(), program.blocks.size(), info.buffers.size(),
                info.images.size(), info.samplers.size(), info.uses_dma,
                static_cast<unsigned>(info.readconst_types), info.has_fetch_shader);

    Shader::Backend::Bindings binding{};
    const std::vector<u32> spirv =
        Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
    const std::string spv_path = out_dir + "/" + name + ".spv";
    WriteFile(spv_path, spirv.data(), spirv.size() * sizeof(u32));
    std::printf("[probe] %s: SPIR-V %zu words -> %s\n", name, spirv.size(), spv_path.c_str());

    spirv_cross::CompilerMSL msl{spirv};
    spirv_cross::CompilerMSL::Options options;
    options.set_msl_version(2, 3);
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    msl.set_msl_options(options);
    const std::string metal = msl.compile();
    const std::string metal_path = out_dir + "/" + name + ".metal";
    WriteFile(metal_path, metal.data(), metal.size());
    std::printf("[probe] %s: MSL %zu bytes -> %s\n", name, metal.size(), metal_path.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <capture-dir> <out-dir>\n", argv[0]);
        return 2;
    }
    const std::string capture = argv[1];
    const std::string out_dir = argv[2];

    std::vector<u8> vs_code = ReadFile(capture + "/draw_largest_vs.bin");
    std::vector<u8> ps_code = ReadFile(capture + "/draw_largest_ps.bin");
    g_regions.push_back({kVsCodeAddr, vs_code});
    g_regions.push_back({kPsCodeAddr, ps_code});
    g_regions.push_back({kFetchAddr, ReadFile(capture + "/draw_largest_fetch.bin")});
    g_regions.push_back(
        {kDescriptorTableAddr, ReadFile(capture + "/draw_largest_vertex_descriptors.bin")});

    Translate("vs", Shader::HwStage::Vertex, Shader::SwStage::Vertex, vs_code, kVsUserData,
              out_dir);
    Translate("ps", Shader::HwStage::Fragment, Shader::SwStage::Fragment, ps_code, kPsUserData,
              out_dir);
    return 0;
}
