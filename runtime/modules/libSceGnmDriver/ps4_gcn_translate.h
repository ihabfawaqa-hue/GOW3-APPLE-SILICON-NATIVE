// SPDX-License-Identifier: GPL-2.0-or-later
// GCN -> MSL translation for the Metal driver, built on the vendored shadPS4 recompiler
// (gpu/shadps4-recompiler). This header is plain C++17 so Objective-C++ callers can include it;
// the implementation (ps4_gcn_translate.cpp) is C++23.
//
// Callers translate a guest stage, then resolve its vertex inputs and resources for the draw
// being encoded. Translate returns null when the stage depends on something this port does not
// handle yet; the caller must then use its heuristic path.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GcnTranslate {

enum class Stage : uint8_t { Vertex, Fragment };

// Pixel-shader state from the draw's SPI registers. Vertex stages ignore it.
struct FragmentState {
    std::array<uint32_t, 32> input_cntl{}; // SPI_PS_INPUT_CNTL_0..31
    uint32_t input_count = 0;              // SPI_PS_IN_CONTROL NUM_INTERP
    uint32_t input_ena = 0;                // SPI_PS_INPUT_ENA
    uint32_t input_addr = 0;               // SPI_PS_INPUT_ADDR
    uint32_t color_export_formats = 0;     // SPI_SHADER_COL_FORMAT, 4 bits per target
    uint32_t z_export_format = 0;          // SPI_SHADER_Z_FORMAT
};

// Guest memory used for shader code and descriptor reads. Call once after the guest is mapped.
void SetGuestMemory(uint8_t *base, size_t size);

// One vertex input, from the stage's fetch shader and the V# it reads at draw time.
// Format values are AmdGpu::DataFormat and AmdGpu::NumberFormat numbers, after the
// remapping the translator applies to the input type.
struct VertexInput {
    uint32_t location;
    uint32_t data_format;
    uint32_t num_format;
    uint32_t stride;
    uint64_t base;
    uint32_t num_records;
    bool per_instance;
    uint32_t input_class; // Type of the shader input: 0 float, 1 uint, 2 int
};

enum class ResourceKind : uint8_t { Buffer, Texture, Sampler };

struct Resource {
    ResourceKind kind;
    uint32_t msl_index; // [[buffer(n)]], [[texture(n)]] or [[sampler(n)]] in the MSL
    uint64_t guest_addr; // Buffers only
    uint64_t guest_bytes; // Buffers only
};

struct Program;

// Translates the stage whose code starts at guest address code_addr. user_data holds the
// stage's 16 user-data SGPRs. vs_param_count is the number of parameter exports of the paired
// vertex stage; the fragment stage uses it as its input count. Pass 0 for vertex stages.
// Returns null and sets *why when the stage is unsupported.
std::shared_ptr<Program> Translate(Stage stage, uint64_t code_addr, const uint32_t *user_data,
                                   uint32_t vs_param_count, const FragmentState &fragment,
                                   std::string *why);

// Identifier unique to this translated program, usable in pipeline cache keys. A retranslation
// of the same code gets a new one, because its MSL may differ.
uint64_t Id(const Program &program);

// MSL source for the stage. Its entry point is named main0.
const std::string &MslSource(const Program &program);

// Number of parameter exports a vertex stage writes to the fragment stage.
uint32_t ParamCount(const Program &program);

// Vertex inputs read from the current descriptors. Only vertex stages have inputs.
std::vector<VertexInput> ResolveVertexInputs(Program &program, const uint32_t *user_data);

// Buffers the stage reads, from the current descriptors. The flattened user-data buffer is not
// included; see ResolveInlineBuffers.
std::vector<Resource> ResolveBuffers(Program &program, const uint32_t *user_data);

// A buffer whose contents the host builds per draw, bound as bytes.
struct InlineBuffer {
    uint32_t msl_index;
    std::vector<uint8_t> bytes;
};

// The flattened user-data buffer (constant reads from user-data pointers), filled from guest
// memory for this draw.
std::vector<InlineBuffer> ResolveInlineBuffers(Program &program, const uint32_t *user_data);

// Diagnostic: the MSL vertex input struct of a vertex stage.
std::string DebugInputStruct(const Program &program);

// Diagnostic: prints the flattened reads, buffer descriptors, and flattened contents.
void DebugDump(Program &program, const uint32_t *user_data);

// MSL buffer index of the push-constant block, or -1 when the stage has none.
int32_t PushIndex(const Program &program);

// Bytes for the push-constant block (the translator's PushData), from the current user data.
// Empty when the stage has no push-constant block.
std::vector<uint8_t> ResolvePushData(Program &program, const uint32_t *user_data);

// A texture the stage samples: its MSL slot and the T# words read for this draw.
struct ImageBinding {
    uint32_t msl_index;
    std::array<uint32_t, 8> words;
    uint32_t info_index = 0;
    uint8_t load_mask = 0;
    std::array<uint16_t, 8> fetch_offsets{};
    std::array<uint32_t, 8> fetch_immediates{};
    std::array<uint32_t, 8> fetch_values{};
};

// Textures the stage samples, from the current descriptors.
std::vector<ImageBinding> ResolveImages(Program &program, const uint32_t *user_data);

// Samplers the stage declares. The caller binds them itself.
std::vector<Resource> DeclaredSamplers(const Program &program);

} // namespace GcnTranslate
