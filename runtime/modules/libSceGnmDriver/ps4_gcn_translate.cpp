// SPDX-License-Identifier: GPL-2.0-or-later
// GCN -> SPIR-V -> MSL translation for the Metal driver. See ps4_gcn_translate.h.
// Built as C++23 because the vendored shadPS4 recompiler requires it.

#include "ps4_gcn_translate.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/types.h"
#include "port/guest_memory.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

#include "spirv_msl.hpp"

namespace {

constexpr u32 kEndpgm = 0xbf810000;
constexpr size_t kMaxCodeDwords = 16384;
constexpr u64 kMinGuestAddr = 0x10000;
// Each descriptor read the translator makes is checked against this many bytes at its base.
constexpr size_t kSnapshotBytes = 1024;
// Programs kept per key. A key is the stage, code, and user-data words.
constexpr size_t kMaxVariantsPerKey = 8;

struct GuestMemory {
    u8 *base = nullptr;
    size_t size = 0;
};

GuestMemory g_guest;
// Reads of unmapped guest memory return zeros, so a bad address yields empty descriptors.
alignas(64) u8 g_zero_page[4096] = {};
// Guest addresses read through GuestToHost while a translation runs.
std::vector<u64> *g_recorded_reads = nullptr;
std::mutex g_mutex;
// Guarded by g_mutex.
u64 g_next_program_id = 1;

// Input classes from the MSL vertex input struct, one line per input:
//   "    int4 vs_in_attr2 [[attribute(2)]];"
// The Metal format must match these types, so the classes are read from the emitted source.
void ParseInputClasses(const std::string &msl, std::unordered_map<u32, u32> &classes) {
    const size_t struct_begin = msl.find("struct main0_in");
    if (struct_begin == std::string::npos) {
        return;
    }
    const size_t struct_end = msl.find("};", struct_begin);
    size_t attr = struct_begin;
    while ((attr = msl.find("[[attribute(", attr)) != std::string::npos && attr < struct_end) {
        const size_t line_start = msl.rfind('\n', attr) + 1;
        const size_t type_begin = msl.find_first_not_of(' ', line_start);
        const std::string type = msl.substr(type_begin, msl.find(' ', type_begin) - type_begin);
        const u32 location = static_cast<u32>(std::strtoul(msl.c_str() + attr + 12, nullptr, 10));
        u32 cls = 0; // float
        if (type.starts_with("uint") || type.starts_with("ushort") || type.starts_with("uchar")) {
            cls = 1;
        } else if (type.starts_with("int") || type.starts_with("short") || type.starts_with("char")) {
            cls = 2;
        }
        classes[location] = cls;
        attr += 12;
    }
}

u64 Fnv1a(const void *data, size_t size, u64 seed = 14695981039346656037ULL) {
    const auto *bytes = static_cast<const u8 *>(data);
    u64 hash = seed;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

u64 HashValue(u64 seed, u64 value) {
    return Fnv1a(&value, sizeof(value), seed);
}

} // namespace

namespace Shader {

const void *GuestToHost(u64 guest_addr) {
    if (!g_guest.base || guest_addr >= g_guest.size) {
        return g_zero_page;
    }
    if (g_recorded_reads) {
        g_recorded_reads->push_back(guest_addr);
    }
    return g_guest.base + guest_addr;
}

} // namespace Shader

namespace GcnTranslate {

struct Program {
    struct Snapshot {
        u64 addr;
        std::array<u8, kSnapshotBytes> bytes;
    };

    // Guest memory the translator read, with its contents at translation time.
    std::vector<Snapshot> snapshots;
    u64 key = 0;
    // Unique per translated program. Metal caches functions and pipelines by this id, so a
    // retranslated variant that shares `key` must not reuse the function of the one before.
    u64 id = 0;
    Stage stage{};
    bool rejected = false;
    std::string reject_reason;
    u32 param_count = 0;
    int32_t push_msl_index = -1;
    std::array<u32, 16> user_data{};
    std::unique_ptr<Shader::Info> info;
    std::string msl;

    // Storage buffers and uniform buffers, by info.buffers index and MSL buffer index.
    struct BufferSlot {
        u32 info_index;
        u32 msl_index;
    };
    std::vector<BufferSlot> buffers;
    struct ImageSlot {
        u32 info_index;
        u32 msl_index;
    };
    std::vector<ImageSlot> images;
    std::vector<Resource> samplers;
    std::vector<Shader::Gcn::VertexAttribute> attributes;
    // Type of each vertex input, by location, from the compiled SPIR-V: 0 float, 1 uint, 2 int.
    std::unordered_map<uint32_t, uint32_t> input_classes;
};

namespace {

std::unordered_map<u64, std::vector<std::shared_ptr<Program>>> g_programs;

// True when every descriptor window the program read still holds the bytes it saw.
bool SnapshotsCurrent(const Program &program) {
    for (const auto &snapshot : program.snapshots) {
        if (snapshot.addr >= g_guest.size) {
            return false;
        }
        const size_t available = std::min<size_t>(kSnapshotBytes, g_guest.size - snapshot.addr);
        if (std::memcmp(g_guest.base + snapshot.addr, snapshot.bytes.data(), available) != 0) {
            return false;
        }
    }
    return true;
}

// Sharp locations the translator could not resolve. Such a sharp reads as a null descriptor,
// so the program is rejected rather than drawn with missing buffers.
template <typename Sharp>
bool HasUnresolvedSharp(const Sharp &resource) {
    return std::ranges::any_of(resource.sharp_fetch.offsets,
                               [](Shader::SharpLocation loc) { return loc == Shader::UNKNOWN_LOCATION; });
}

} // namespace

void SetGuestMemory(u8 *base, size_t size) {
    std::lock_guard lock(g_mutex);
    g_guest = GuestMemory{base, size};
}

std::shared_ptr<Program> Translate(Stage stage, u64 code_addr, const u32 *user_data,
                                   u32 vs_param_count, const FragmentState &fragment,
                                   std::string *why) {
    std::lock_guard lock(g_mutex);
    auto fail = [&](const std::string &reason) {
        if (why) {
            *why = reason;
        }
        return std::shared_ptr<Program>{};
    };

    if (!g_guest.base) {
        return fail("guest memory not set");
    }
    if (code_addr < kMinGuestAddr || code_addr >= g_guest.size) {
        return fail("code address outside guest memory");
    }

    // The stage's code runs to its first s_endpgm.
    const auto *code_ptr = reinterpret_cast<const u32 *>(g_guest.base + code_addr);
    const size_t max_dwords =
        std::min<size_t>(kMaxCodeDwords, (g_guest.size - code_addr) / sizeof(u32));
    size_t dwords = 0;
    while (dwords < max_dwords && code_ptr[dwords] != kEndpgm) {
        dwords++;
    }
    if (dwords == max_dwords) {
        return fail("no s_endpgm within the code window");
    }
    dwords++;

    u64 key = HashValue(static_cast<u64>(stage), code_addr);
    key = HashValue(key, dwords);
    key = HashValue(key, vs_param_count);
    key = Fnv1a(user_data, 16 * sizeof(u32), key);
    if (stage == Stage::Fragment) {
        key = Fnv1a(fragment.input_cntl.data(), sizeof(fragment.input_cntl), key);
        key = HashValue(key, fragment.input_count);
        key = HashValue(key, fragment.input_ena);
        key = HashValue(key, fragment.input_addr);
        key = HashValue(key, fragment.color_export_formats);
        key = HashValue(key, fragment.z_export_format);
    }

    auto &variants = g_programs[key];
    for (const auto &variant : variants) {
        if (SnapshotsCurrent(*variant)) {
            if (variant->rejected) {
                return fail(variant->reject_reason);
            }
            return variant;
        }
    }

    auto program = std::make_shared<Program>();
    program->key = key;
    program->id = g_next_program_id++;
    program->stage = stage;
    program->param_count = vs_param_count;
    std::copy_n(user_data, program->user_data.size(), program->user_data.begin());

    // Record the guest reads this translation makes, so a later draw can tell whether they
    // still hold the same bytes. Every exit below goes through remember(), and the scope
    // clears the recorder on every exit path.
    std::vector<u64> reads;
    struct RecordScope {
        explicit RecordScope(std::vector<u64> *target) {
            g_recorded_reads = target;
        }
        ~RecordScope() {
            g_recorded_reads = nullptr;
        }
    } record_scope{&reads};

    // Stores the program (accepted or rejected) with the descriptor windows it read.
    auto remember = [&](std::shared_ptr<Program> result) {
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        for (const u64 addr : reads) {
            if (addr >= g_guest.size) {
                continue;
            }
            Program::Snapshot snapshot{addr, {}};
            const size_t available = std::min<size_t>(kSnapshotBytes, g_guest.size - addr);
            std::memcpy(snapshot.bytes.data(), g_guest.base + addr, available);
            result->snapshots.push_back(snapshot);
        }
        if (variants.size() >= kMaxVariantsPerKey) {
            variants.erase(variants.begin());
        }
        variants.push_back(result);
        return result;
    };
    auto reject = [&](const std::string &reason) {
        program->rejected = true;
        program->reject_reason = reason;
        remember(program);
        return fail(reason);
    };

    try {
        Common::Assert::ThrowOnFailure guard;
        const auto hw_stage = stage == Stage::Vertex ? Shader::HwStage::Vertex
                                                     : Shader::HwStage::Fragment;
        const auto sw_stage = stage == Stage::Vertex ? Shader::SwStage::Vertex
                                                     : Shader::SwStage::Fragment;
        const std::span<const u32> code{code_ptr, dwords};
        const Shader::ShaderParams params{
            .user_data = std::span<const u32, 16>{program->user_data},
            .code = code,
            .hash = Fnv1a(code.data(), code.size_bytes()),
        };
        program->info = std::make_unique<Shader::Info>(hw_stage, sw_stage, params);

        Shader::RuntimeInfo runtime_info{};
        runtime_info.sw_stage = sw_stage;
        runtime_info.hw_stage = hw_stage;
        runtime_info.props.num_user_data = 16;
        if (stage == Stage::Fragment) {
            // Pixel-shader inputs and enables come from the draw's SPI registers. The translator
            // and Metal link PS input i to VS parameter input_offset, so the emitter binds each
            // input at its parameter index.
            const u32 input_count = fragment.input_count;
            if (input_count > vs_param_count) {
                return reject("pixel shader reads more inputs than the vertex shader exports");
            }
            // Depth export format. The translator handles only uncompressed 32-bit formats; any
            // other value means no depth export, and mrtz_mask is zero, so R_32 is harmless.
            const auto z_format = static_cast<AmdGpu::ShaderExportFormat>(fragment.z_export_format & 0xF);
            const bool z_uncompressed = z_format == AmdGpu::ShaderExportFormat::R_32 ||
                                        z_format == AmdGpu::ShaderExportFormat::GR_32 ||
                                        z_format == AmdGpu::ShaderExportFormat::AR_32 ||
                                        z_format == AmdGpu::ShaderExportFormat::ABGR_32;
            runtime_info.hw.fs.z_export_format =
                z_uncompressed ? z_format : AmdGpu::ShaderExportFormat::R_32;
            std::memcpy(&runtime_info.hw.fs.en_flags, &fragment.input_ena, sizeof(u32));
            std::memcpy(&runtime_info.hw.fs.addr_flags, &fragment.input_addr, sizeof(u32));
            runtime_info.hw.fs.num_inputs = static_cast<s32>(input_count);
            for (u32 i = 0; i < input_count; i++) {
                AmdGpu::PsInputControl control{};
                std::memcpy(&control, &fragment.input_cntl[i], sizeof(u32));
                auto &input = runtime_info.hw.fs.inputs[i];
                input.param_index = static_cast<u8>(control.input_offset);
                input.is_default = control.use_default;
                input.is_flat = control.flat_shade;
                input.default_value = static_cast<u8>(control.default_value);
            }
            // Color targets: export format from SPI_SHADER_COL_FORMAT. Data and number formats
            // stay R8G8B8A8 unorm until the CB_COLOR formats are decoded.
            for (u32 target = 0; target < 8; target++) {
                const auto export_format = static_cast<AmdGpu::ShaderExportFormat>(
                    (fragment.color_export_formats >> (4 * target)) & 0xF);
                if (export_format == AmdGpu::ShaderExportFormat::Zero) {
                    continue;
                }
                auto &mrt = runtime_info.hw.fs.color_buffers[target];
                mrt.data_format = AmdGpu::DataFormat::Format8_8_8_8;
                mrt.num_format = AmdGpu::NumberFormat::Unorm;
                mrt.export_format = export_format;
                mrt.swizzle.array = {AmdGpu::CompSwizzle::Red, AmdGpu::CompSwizzle::Green,
                                     AmdGpu::CompSwizzle::Blue, AmdGpu::CompSwizzle::Alpha};
            }
        }

        Shader::Profile profile{};
        profile.supported_spirv = 0x00010000;
        profile.subgroup_size = 32;

        static Shader::Pools pools;
        Shader::IR::Program ir =
            Shader::TranslateProgram(code, pools, *program->info, runtime_info, profile);
        const Shader::Info &info = *program->info;

        if (info.uses_dma) {
            return reject("uses direct memory access");
        }
        // Constant reads with a dynamic index or a nested base have no flattened slot.
        if (info.flat_user_data_unresolved) {
            return reject("constant read with a dynamic index or nested base");
        }
        // Descriptors loaded from the flattened buffer are fine: the host fills it per draw.
        for (const auto &buffer : info.buffers) {
            if (HasUnresolvedSharp(buffer) ||
                buffer.post_op == Shader::SharpFetchPostOp::OffsetByProgramBase) {
                return reject("buffer descriptor source is not resolved");
            }
        }
        for (const auto &image : info.images) {
            if (HasUnresolvedSharp(image) ||
                image.post_op == Shader::SharpFetchPostOp::OffsetByProgramBase) {
                return reject("image descriptor source is not resolved");
            }
        }

        Shader::Backend::Bindings binding{};
        const std::vector<u32> spirv =
            Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir, binding);

        spirv_cross::CompilerMSL compiler{spirv};
        spirv_cross::CompilerMSL::Options options;
        options.set_msl_version(2, 3);
        options.platform = spirv_cross::CompilerMSL::Options::macOS;
        compiler.set_msl_options(options);
        program->msl = compiler.compile();

        // SPIR-V buffer binding k is info.buffers[k]; see DefineBuffers in the emitter.
        const auto resources = compiler.get_shader_resources();
        auto add_buffers = [&](const auto &list) {
            for (const auto &resource : list) {
                const u32 binding_index = compiler.get_decoration(resource.id, spv::DecorationBinding);
                if (binding_index >= info.buffers.size()) {
                    return false;
                }
                program->buffers.push_back(
                    {binding_index, compiler.get_automatic_msl_resource_binding(resource.id)});
            }
            return true;
        };
        if (!add_buffers(resources.storage_buffers) || !add_buffers(resources.uniform_buffers)) {
            return reject("buffer binding outside info.buffers");
        }
        if (!resources.push_constant_buffers.empty()) {
            program->push_msl_index =
                static_cast<int32_t>(compiler.get_automatic_msl_resource_binding(resources.push_constant_buffers[0].id));
        }
        // Image i takes SPIR-V bindings [first, first + NumBindings) in info.images order, and
        // samplers follow the last image (DefineImagesAndSamplers in the emitter).
        std::vector<std::pair<u32, u32>> image_ranges;
        u32 next_binding = static_cast<u32>(info.buffers.size());
        for (const auto &image : info.images) {
            const u32 count = image.NumBindings(info);
            image_ranges.push_back({next_binding, count});
            next_binding += count;
        }
        const u32 first_sampler_binding = next_binding;

        if (!resources.sampled_images.empty()) {
            return reject("combined image samplers are not translated");
        }
        for (const auto &resource : resources.separate_images) {
            const u32 binding_index = compiler.get_decoration(resource.id, spv::DecorationBinding);
            const auto range = std::ranges::find_if(image_ranges, [&](const auto &r) {
                return binding_index >= r.first && binding_index < r.first + r.second;
            });
            if (range == image_ranges.end()) {
                return reject("image binding outside info.images");
            }
            const u32 image_index = static_cast<u32>(range - image_ranges.begin());
            if (info.images[image_index].is_written) {
                return reject("storage images are not translated");
            }
            const auto &type = compiler.get_type(resource.base_type_id);
            if (type.image.dim != spv::Dim2D || type.image.arrayed || type.image.ms ||
                compiler.get_type(type.image.type).basetype != spirv_cross::SPIRType::Float) {
                if (std::getenv("GOW3_GCN_DEBUG")) {
                    const auto sharp = info.images[image_index].GetSharp(info);
                    std::array<u32, 8> words{};
                    std::memcpy(words.data(), &sharp, sizeof(sharp));
                    std::fprintf(stderr,
                                 "[gcn-debug] image %u T# type=%u words=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                                 image_index, static_cast<u32>(sharp.GetType()), words[0], words[1],
                                 words[2], words[3], words[4], words[5], words[6], words[7]);
                }
                if (type.image.ms) {
                    return reject("multisampled textures are not translated");
                }
                if (compiler.get_type(type.image.type).basetype != spirv_cross::SPIRType::Float) {
                    return reject("only float textures are translated");
                }
                return reject("only 2D textures are translated");
            }
            program->images.push_back(
                {image_index, compiler.get_automatic_msl_resource_binding(resource.id)});
        }
        for (const auto &resource : resources.separate_samplers) {
            if (compiler.get_decoration(resource.id, spv::DecorationBinding) < first_sampler_binding) {
                return reject("sampler binding outside info.samplers");
            }
            program->samplers.push_back(
                {ResourceKind::Sampler, compiler.get_automatic_msl_resource_binding(resource.id), 0, 0});
        }

        if (stage == Stage::Vertex) {
            program->param_count = 0;
            for (size_t pos = program->msl.find("[[user(locn"); pos != std::string::npos;
                 pos = program->msl.find("[[user(locn", pos + 1)) {
                program->param_count++;
            }
            if (const auto fetch = Shader::Gcn::ParseFetchShader(info)) {
                program->attributes = fetch->attributes;
            }
            ParseInputClasses(program->msl, program->input_classes);
        }

        remember(program);
        return program;
    } catch (const std::exception &e) {
        return reject(std::string("translator error: ") + e.what());
    }
}

u64 Id(const Program &program) {
    return program.id;
}

const std::string &MslSource(const Program &program) {
    return program.msl;
}

u32 ParamCount(const Program &program) {
    return program.param_count;
}

int32_t PushIndex(const Program &program) {
    return program.push_msl_index;
}

std::string DebugInputStruct(const Program &program) {
    const std::string &msl = program.msl;
    const size_t begin = msl.find("struct main0_in");
    if (begin == std::string::npos) {
        return {};
    }
    return msl.substr(begin, msl.find("};", begin) - begin + 2);
}

std::vector<VertexInput> ResolveVertexInputs(Program &program, const u32 *user_data) {
    std::copy_n(user_data, program.user_data.size(), program.user_data.begin());
    std::vector<VertexInput> inputs;
    inputs.reserve(program.attributes.size());
    for (const auto &attribute : program.attributes) {
        // The V# is read from the current user data, so the input follows the descriptor.
        const AmdGpu::Buffer sharp = attribute.GetSharp(*program.info);
        const auto cls = program.input_classes.find(attribute.semantic);
        const u32 input_class = cls != program.input_classes.end() ? cls->second : 0u;
        inputs.push_back({
            .location = attribute.semantic,
            .data_format = static_cast<u32>(sharp.GetDataFmt()),
            .num_format = static_cast<u32>(sharp.GetNumberFmt()),
            .stride = sharp.stride,
            .base = sharp.base_address,
            .num_records = sharp.num_records,
            .per_instance =
                attribute.GetStepRate() != Shader::Gcn::VertexAttribute::InstanceIdType::None,
            .input_class = input_class,
        });
    }
    return inputs;
}

// Fills the flattened user-data buffer for this draw: user data first, then each constant read
// from guest memory. Constant reads of unmapped memory yield zero.
void FillFlatBuffer(Program &program, const u32 *user_data) {
    std::copy_n(user_data, program.user_data.size(), program.user_data.begin());
    Shader::Info &info = *program.info;
    auto &flat = info.flattened_ud_buf;
    std::copy_n(user_data, Shader::NUM_USER_DATA_REGS, flat.begin());
    for (const auto &read : info.flat_user_data_reads) {
        const u64 ptr = ((static_cast<u64>(user_data[read.ud_lo + 1]) << 32) |
                         user_data[read.ud_lo]) & 0xFFFFFFFFFFFFULL;
        const u64 addr = ptr + static_cast<u64>(read.dword_offset) * sizeof(u32);
        u32 value = 0;
        if (addr >= kMinGuestAddr && addr + sizeof(u32) <= g_guest.size) {
            std::memcpy(&value, g_guest.base + addr, sizeof(value));
        }
        flat[read.slot] = value;
    }
}

void DebugDump(Program &program, const u32 *user_data) {
    FillFlatBuffer(program, user_data);
    const Shader::Info &info = *program.info;
    std::fprintf(stderr, "[gcn-debug] stage=%s flatbuf_dw=%zu flat_reads=%zu unresolved=%d\n",
                 program.stage == Stage::Vertex ? "vs" : "ps", info.flattened_ud_buf.size(),
                 info.flat_user_data_reads.size(), info.flat_user_data_unresolved ? 1 : 0);
    for (const auto &read : info.flat_user_data_reads) {
        std::fprintf(stderr, "[gcn-debug]   read ud%u+%u -> slot %u = 0x%08x\n", read.ud_lo,
                     read.dword_offset, read.slot, info.flattened_ud_buf[read.slot]);
    }
    for (size_t i = 0; i < info.buffers.size(); i++) {
        const auto &buffer = info.buffers[i];
        const AmdGpu::Buffer sharp = buffer.GetSharp(info);
        std::fprintf(stderr,
                     "[gcn-debug]   buffer %zu type=%d used=0x%x load_mask=0x%x sharp valid=%d "
                     "base=0x%llx stride=%u records=%u dfmt=%u nfmt=%u\n",
                     i, static_cast<int>(buffer.buffer_type), static_cast<unsigned>(buffer.used_types),
                     buffer.sharp_fetch.load_mask, sharp.Valid() ? 1 : 0,
                     (unsigned long long)sharp.base_address, sharp.stride, sharp.num_records,
                     static_cast<unsigned>(sharp.GetDataFmt()), static_cast<unsigned>(sharp.GetNumberFmt()));
    }
}

std::vector<InlineBuffer> ResolveInlineBuffers(Program &program, const u32 *user_data) {
    FillFlatBuffer(program, user_data);
    std::vector<InlineBuffer> inlines;
    for (const auto &slot : program.buffers) {
        if (program.info->buffers[slot.info_index].buffer_type != Shader::BufferType::Flatbuf) {
            continue;
        }
        const auto &flat = program.info->flattened_ud_buf;
        InlineBuffer inline_buffer{slot.msl_index, std::vector<uint8_t>(flat.size() * sizeof(u32))};
        std::memcpy(inline_buffer.bytes.data(), flat.data(), inline_buffer.bytes.size());
        inlines.push_back(std::move(inline_buffer));
    }
    return inlines;
}

std::vector<Resource> ResolveBuffers(Program &program, const u32 *user_data) {
    FillFlatBuffer(program, user_data);
    std::vector<Resource> resources;
    resources.reserve(program.buffers.size());
    for (const auto &slot : program.buffers) {
        if (program.info->buffers[slot.info_index].buffer_type == Shader::BufferType::Flatbuf) {
            continue; // Bound through ResolveInlineBuffers
        }
        const AmdGpu::Buffer sharp = program.info->buffers[slot.info_index].GetSharp(*program.info);
        u64 bytes = 0;
        if (sharp.Valid()) {
            bytes = static_cast<u64>(std::max<u32>(sharp.stride, 1)) * sharp.num_records;
        }
        resources.push_back({ResourceKind::Buffer, slot.msl_index, sharp.base_address, bytes});
    }
    return resources;
}

std::vector<uint8_t> ResolvePushData(Program &program, const u32 *user_data) {
    std::copy_n(user_data, program.user_data.size(), program.user_data.begin());
    if (program.push_msl_index < 0) {
        return {};
    }
    // Buffer byte offsets stay zero: the host binds each buffer at its guest base address.
    Shader::PushData push{};
    push.xscale = 1.0f;
    push.yscale = 1.0f;
    Shader::Backend::Bindings bindings{};
    program.info->PushUd(bindings, push);
    std::vector<uint8_t> bytes(sizeof(push));
    std::memcpy(bytes.data(), &push, sizeof(push));
    return bytes;
}

std::vector<ImageBinding> ResolveImages(Program &program, const u32 *user_data) {
    FillFlatBuffer(program, user_data);
    std::vector<ImageBinding> bindings;
    bindings.reserve(program.images.size());
    for (const auto &slot : program.images) {
        const auto &resource = program.info->images[slot.info_index];
        const AmdGpu::Image sharp = resource.GetSharp(*program.info);
        static_assert(sizeof(AmdGpu::Image) == 8 * sizeof(u32));
        ImageBinding binding{slot.msl_index, {}};
        binding.info_index = slot.info_index;
        binding.load_mask = resource.sharp_fetch.load_mask;
        binding.fetch_offsets = resource.sharp_fetch.offsets;
        binding.fetch_immediates = resource.sharp_fetch.immediates;
        for (size_t i = 0; i < binding.fetch_values.size(); ++i) {
            const auto offset = binding.fetch_offsets[i];
            binding.fetch_values[i] = offset != Shader::UNKNOWN_LOCATION &&
                                      offset < program.info->flattened_ud_buf.size()
                                          ? program.info->flattened_ud_buf[offset]
                                          : binding.fetch_immediates[i];
        }
        std::memcpy(binding.words.data(), &sharp, sizeof(sharp));
        bindings.push_back(binding);
    }
    return bindings;
}

std::vector<Resource> DeclaredSamplers(const Program &program) {
    return program.samplers;
}

} // namespace GcnTranslate
