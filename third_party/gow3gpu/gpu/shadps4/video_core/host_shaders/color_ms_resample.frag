// SPDX-License-Identifier: GPL-2.0-or-later
// Resize each MSAA sample independently. No averaging or sample-count conversion.
#version 450 core
#extension GL_EXT_samplerless_texture_functions : require
layout(binding=0, set=0) uniform texture2DMS source_color;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() {
    ivec2 size = textureSize(source_color);
    ivec2 coord = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
#ifdef TEST_PATTERN
    color = gl_SampleID == 0 ? vec4(1,0,0,1) : vec4(0,1,0,1);
#else
    color = texelFetch(source_color, coord, gl_SampleID);
#endif
}
