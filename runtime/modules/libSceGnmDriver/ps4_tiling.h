// SPDX-License-Identifier: GPL-2.0-or-later
// GCN tiled surface addressing for the Metal texture path. The address math follows the tiler in
// shadPS4 (src/video_core/host_shaders/tiling.comp, GPL-2.0-or-later): thin and display micro tiles
// in 1D and 2D layouts, with 8 pipes (P8_32x32_16x16), 1 bank wide, and no bank swizzle. Tile modes
// outside that set are reported by IsSupported, and the caller keeps the linear upload for them.
//
// Not verified against hardware: the 256-byte pipe interleave comes from the same shader and has not
// been checked against a surface that renders correctly. SelfTest checks only that each layout maps
// its elements to distinct addresses inside its span.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Ps4Tiling {

// T# tiling_index values this file handles. Depth2DThin64 .. Depth2DThin1K are 0..4.
constexpr uint32_t kTileModeDepth2DThin1K = 4;
constexpr uint32_t kTileModeDisplay2DThin = 10;
constexpr uint32_t kTileModeThin1DThin = 13;
constexpr uint32_t kTileModeThin2DThin = 14;

constexpr uint32_t kMicroTileDim = 8;
constexpr uint32_t kNumPipes = 8;
constexpr uint32_t kNumPipeBits = 3;
constexpr uint32_t kPipeInterleaveBits = 8;
constexpr uint32_t kPipeInterleaveBytes = 1u << kPipeInterleaveBits;

struct MacroMode {
    uint32_t bank_height;
    uint32_t num_banks;
    uint32_t aspect;
};

// Macro tile modes 0..15 (AmdGpu::MacroTileMode), without the PRT variants.
constexpr MacroMode kMacroModes[16] = {
    {4, 16, 4}, {2, 16, 2}, {1, 16, 2}, {1, 16, 2}, {1, 8, 1}, {1, 4, 1}, {1, 2, 1}, {1, 2, 1},
    {8, 16, 4}, {4, 16, 4}, {2, 16, 2}, {1, 16, 2}, {1, 8, 1}, {1, 4, 1}, {1, 2, 1}, {1, 2, 1},
};

struct Layout {
    uint32_t elem_bits = 0;
    bool macro = true;   // false: 1D micro tiling, with no pipes or banks
    bool display = false; // display micro tile order instead of thin
    uint32_t bank_bits = 0;
    uint32_t bank_height = 1;
    uint32_t num_banks = 16;
    uint32_t macro_pitch = 0;  // elements
    uint32_t macro_height = 0; // elements
    uint32_t macro_bytes = 0;  // bytes one macro tile takes in the tiled offset space
    uint32_t tile_bytes = 0;   // bytes of one micro tile after any split
    uint32_t split_bytes = 0;  // TILE_SPLIT_BYTES
    uint32_t slices_per_tile = 1;
};

inline bool ComputeLayout(uint32_t tile_mode, uint32_t elem_bits, Layout &out) {
    if (elem_bits < 8 || elem_bits > 128 || elem_bits % 8 != 0) return false;
    const uint32_t micro_bytes = kMicroTileDim * kMicroTileDim * elem_bits / 8;
    const bool depth = tile_mode <= kTileModeDepth2DThin1K;
    const bool display = tile_mode == kTileModeDisplay2DThin;

    out = Layout{};
    out.elem_bits = elem_bits;
    out.display = display;
    out.tile_bytes = micro_bytes;
    out.split_bytes = micro_bytes;
    if (tile_mode == kTileModeThin1DThin) {
        out.macro = false;
        return true;
    }
    if (!depth && !display && tile_mode != kTileModeThin2DThin) return false;

    const uint32_t sample_split = depth ? 1u : 2u;
    const uint32_t tile_split_hw = 64u << tile_mode; // depth modes: 64, 128, 256, 512, 1024
    const uint32_t color_split = std::max(256u, sample_split * micro_bytes);
    const uint32_t split = std::min(1024u, depth ? tile_split_hw : color_split);

    // One sample, so the macro mode comes from the tile bytes alone.
    const uint32_t mode_bytes = std::min(split, micro_bytes);
    uint32_t mode_index = 0;
    for (uint32_t v = mode_bytes / 64; v > 1; v >>= 1) ++mode_index;
    const MacroMode mode = kMacroModes[mode_index];

    out.bank_height = mode.bank_height;
    out.num_banks = mode.num_banks;
    while ((1u << out.bank_bits) < out.num_banks) ++out.bank_bits;
    out.macro_pitch = kMicroTileDim * kNumPipes * mode.aspect; // bank width is 1
    out.macro_height = kMicroTileDim * mode.bank_height * mode.num_banks / mode.aspect;

    out.split_bytes = split;
    if (micro_bytes > split) {
        out.slices_per_tile = micro_bytes / split;
        out.tile_bytes = split;
    }
    out.macro_bytes = out.tile_bytes * (out.macro_pitch / kMicroTileDim) *
                      (out.macro_height / kMicroTileDim) / (kNumPipes * mode.num_banks);
    return out.macro_bytes > 0;
}

inline bool IsSupported(uint32_t tile_mode, uint32_t elem_bits) {
    Layout layout;
    return ComputeLayout(tile_mode, elem_bits, layout);
}

// A tiled surface: its layout plus the tile counts for one padded slice.
struct Surface {
    Layout layout;
    uint32_t tiles_per_row = 0;  // macro tiles for macro layouts, micro tiles for 1D
    uint64_t tiles_per_slice = 0;
    uint64_t span = 0; // bytes of tiled memory the surface can address
};

inline bool MakeSurface(uint32_t tile_mode, uint32_t elem_bits, uint32_t pitch_elems, uint32_t rows,
                        Surface &out) {
    if (!ComputeLayout(tile_mode, elem_bits, out.layout)) return false;
    const Layout &L = out.layout;
    const uint32_t tile_w = L.macro ? L.macro_pitch : kMicroTileDim;
    const uint32_t tile_h = L.macro ? L.macro_height : kMicroTileDim;
    const uint32_t aligned_pitch = (pitch_elems + tile_w - 1) / tile_w * tile_w;
    const uint32_t padded_rows = (rows + tile_h - 1) / tile_h * tile_h;
    out.tiles_per_row = aligned_pitch / tile_w;
    out.tiles_per_slice = (uint64_t)out.tiles_per_row * (padded_rows / tile_h);
    if (!L.macro) {
        out.span = out.tiles_per_slice * L.tile_bytes;
        return true;
    }
    const uint64_t logical = out.tiles_per_slice * L.macro_bytes * L.slices_per_tile;
    const uint64_t chunks = (logical + kPipeInterleaveBytes - 1) / kPipeInterleaveBytes;
    out.span = chunks << (kPipeInterleaveBits + kNumPipeBits + L.bank_bits);
    return true;
}

inline uint32_t Bit(uint32_t value, uint32_t index) {
    return (value >> index) & 1u;
}

// Element index within an 8x8 micro tile. Thin and depth order the bits x0 y0 x1 y1 x2 y2 from the
// low bit up. Display orders them by element size, as the tiler's per-size cases do.
inline uint32_t PixelIndex(const Layout &L, uint32_t px, uint32_t py) {
    struct Source {
        uint32_t axis; // 0 takes the x coordinate, 1 the y coordinate
        uint32_t bit;
    };
    static const Source kThin[6] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}, {1, 2}};
    static const Source kDisplay8[6] = {{0, 0}, {0, 1}, {0, 2}, {1, 1}, {1, 0}, {1, 2}};
    static const Source kDisplay16[6] = {{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}};
    static const Source kDisplay32[6] = {{0, 0}, {0, 1}, {1, 0}, {0, 2}, {1, 1}, {1, 2}};
    static const Source kDisplay64[6] = {{0, 0}, {1, 0}, {0, 1}, {0, 2}, {1, 1}, {1, 2}};
    static const Source kDisplay128[6] = {{1, 0}, {0, 0}, {0, 1}, {0, 2}, {1, 1}, {1, 2}};

    const Source *order = kThin;
    if (L.display) {
        switch (L.elem_bits) {
        case 8: order = kDisplay8; break;
        case 16: order = kDisplay16; break;
        case 32: order = kDisplay32; break;
        case 64: order = kDisplay64; break;
        default: order = kDisplay128; break;
        }
    }
    uint32_t index = 0;
    for (uint32_t i = 0; i < 6; ++i) {
        index |= Bit(order[i].axis ? py : px, order[i].bit) << i;
    }
    return index;
}

// Byte offset of element (x, y) in the tiled surface.
inline uint64_t TiledOffset(const Surface &s, uint32_t x, uint32_t y) {
    const Layout &L = s.layout;
    const uint64_t element =
        (uint64_t)PixelIndex(L, x % kMicroTileDim, y % kMicroTileDim) * L.elem_bits / 8;

    if (!L.macro) {
        const uint64_t tile_index =
            (uint64_t)(y / kMicroTileDim) * s.tiles_per_row + x / kMicroTileDim;
        return tile_index * L.tile_bytes + element;
    }

    uint64_t offset_element = element;
    uint32_t split_slice = 0;
    if (L.slices_per_tile > 1) {
        split_slice = (uint32_t)(offset_element / L.split_bytes);
        offset_element %= L.split_bytes;
    }

    const uint64_t slice_bytes = s.tiles_per_slice * L.macro_bytes;
    const uint64_t macro_index = (uint64_t)(y / L.macro_height) * s.tiles_per_row + x / L.macro_pitch;
    const uint64_t tile = (uint64_t)((y / kMicroTileDim) % L.bank_height) * L.tile_bytes;
    const uint64_t total =
        split_slice * slice_bytes + macro_index * L.macro_bytes + offset_element + tile;

    const uint32_t tx = x / kMicroTileDim;
    const uint32_t ty = y / kMicroTileDim;
    const uint32_t pipe = (Bit(tx, 0) ^ Bit(ty, 0) ^ Bit(tx, 1)) |
                          ((Bit(tx, 1) ^ Bit(ty, 1)) << 1) | ((Bit(tx, 2) ^ Bit(ty, 2)) << 2);

    const uint32_t bx = tx / kNumPipes;
    const uint32_t by = ty / L.bank_height;
    uint32_t bank = 0;
    switch (L.num_banks) {
    case 16:
        bank = (Bit(bx, 0) ^ Bit(by, 3)) | ((Bit(bx, 1) ^ Bit(by, 2) ^ Bit(by, 3)) << 1) |
               ((Bit(bx, 2) ^ Bit(by, 1)) << 2) | ((Bit(bx, 3) ^ Bit(by, 0)) << 3);
        break;
    case 8:
        bank = (Bit(bx, 0) ^ Bit(by, 2)) | ((Bit(bx, 1) ^ Bit(by, 1) ^ Bit(by, 2)) << 1) |
               ((Bit(bx, 2) ^ Bit(by, 0)) << 2);
        break;
    case 4:
        bank = (Bit(bx, 0) ^ Bit(by, 1)) | ((Bit(bx, 1) ^ Bit(by, 0)) << 1);
        break;
    default:
        bank = Bit(bx, 0) ^ Bit(by, 0);
        break;
    }
    // Bank swizzle is zero: the captured descriptors leave alt_tile_mode clear.
    bank ^= (L.num_banks / 2 + 1) * split_slice;
    bank &= L.num_banks - 1;

    const uint64_t chunk = total >> kPipeInterleaveBits;
    return (total & (kPipeInterleaveBytes - 1)) | ((uint64_t)pipe << kPipeInterleaveBits) |
           ((uint64_t)bank << (kPipeInterleaveBits + kNumPipeBits)) |
           (chunk << (kPipeInterleaveBits + kNumPipeBits + L.bank_bits));
}

// Copies a tiled surface's mip 0 into linear rows. cols and rows count elements (blocks for
// compressed formats), and pitch_elems is the tiled row pitch in elements. Returns false when the
// tile mode is unsupported or the source is too small for the surface.
inline bool Detile(const uint8_t *src, size_t src_bytes, uint32_t tile_mode, uint32_t elem_bits,
                   uint32_t pitch_elems, uint32_t cols, uint32_t rows, uint8_t *dst,
                   size_t dst_row_bytes) {
    Surface s;
    if (!MakeSurface(tile_mode, elem_bits, pitch_elems, rows, s)) return false;
    if (cols > pitch_elems || s.span > src_bytes) return false;
    const size_t elem_bytes = elem_bits / 8;
    for (uint32_t y = 0; y < rows; ++y) {
        uint8_t *row = dst + y * dst_row_bytes;
        for (uint32_t x = 0; x < cols; ++x) {
            std::memcpy(row + x * elem_bytes, src + TiledOffset(s, x, y), elem_bytes);
        }
    }
    return true;
}

// Checks that every element of a surface maps to its own address inside its span. A layout that
// fails this cannot be right, whatever the console does.
inline bool SelfTest(uint32_t tile_mode, uint32_t elem_bits, uint32_t pitch_elems, uint32_t cols,
                     uint32_t rows) {
    Surface s;
    if (!MakeSurface(tile_mode, elem_bits, pitch_elems, rows, s)) return false;
    const size_t elem_bytes = elem_bits / 8;
    std::vector<bool> seen(s.span / elem_bytes, false);
    for (uint32_t y = 0; y < rows; ++y) {
        for (uint32_t x = 0; x < cols; ++x) {
            const uint64_t offset = TiledOffset(s, x, y);
            if (offset % elem_bytes != 0 || offset >= s.span) return false;
            const size_t index = offset / elem_bytes;
            if (seen[index]) return false;
            seen[index] = true;
        }
    }
    return true;
}

} // namespace Ps4Tiling
