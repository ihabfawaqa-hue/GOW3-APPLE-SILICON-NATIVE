// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: nearest resample of a depth/stencil image between native and reduced scene
// targets. Used where the format has no blit support (D32S8 on RADV).

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require
#if defined(HAS_STENCIL) || defined(STENCIL_BITS)
#if defined(HAS_STENCIL)
#extension GL_ARB_shader_stencil_export : require
#endif
#ifdef MULTISAMPLE
layout (binding = 1, set = 0) uniform utexture2DMS src_stencil;
#else
layout (binding = 1, set = 0) uniform utexture2D src_stencil;
#endif
#endif
#if defined(STENCIL_BITS)
layout (push_constant) uniform StencilBit { uint bit; } pc;
#endif

#ifdef MULTISAMPLE
layout (binding = 0, set = 0) uniform texture2DMS src_depth;
#define SOURCE_SIZE textureSize(src_depth)
#define SOURCE_SAMPLE gl_SampleID
#else
layout (binding = 0, set = 0) uniform texture2D src_depth;
#define SOURCE_SIZE textureSize(src_depth, 0)
#define SOURCE_SAMPLE 0
#endif

layout (location = 0) in vec2 uv;

void main() {
    const ivec2 size = SOURCE_SIZE;
    const ivec2 coord = min(ivec2(uv * vec2(size)), size - 1);
#if defined(STENCIL_BITS)
    // Fixed-function stencil replacement writes one bit per draw. The first draw
    // (bit=0, writeMask=0) copies depth everywhere, including zero-stencil pixels.
    if (pc.bit != 0u && (texelFetch(src_stencil, coord, SOURCE_SAMPLE).r & pc.bit) == 0u) discard;
#endif
    gl_FragDepth = texelFetch(src_depth, coord, SOURCE_SAMPLE).r;
#if defined(HAS_STENCIL)
    gl_FragStencilRefARB = int(texelFetch(src_stencil, coord, SOURCE_SAMPLE).r);
#endif
}
