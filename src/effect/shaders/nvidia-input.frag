// SPDX-License-Identifier: GPL-2.0-or-later
#version 140
#include "kwin-vfx/cas.glsl"
uniform sampler2D sampler;
uniform float preCasStrength;
uniform vec2 texelSize;
in vec2 texcoord0;
out vec4 fragColor;
void main()
{
    vec4 p = casSharpen(sampler, texcoord0, texelSize, preCasStrength);
    // VFX sees straight SDR RGB with opaque alpha. The final draw preserves
    // alpha from the original capture and premultiplies the processed RGB.
    fragColor = vec4(p.a > 0.0001 ? p.rgb / p.a : vec3(0.0), 1.0);
}
