// SPDX-License-Identifier: GPL-2.0-or-later
#version 140

#include "colormanagement.glsl"
#include "saturation.glsl"

uniform sampler2D sampler;
uniform vec4 modulation;
in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 tex = sourceEncodingToNitsInDestinationColorspace(texture(sampler, texcoord0));
    tex = adjustSaturation(tex);
    // Deliberately obvious cyan tint for selection validation. Preserve alpha.
    tex.rgb *= vec3(0.25, 1.0, 1.0);
    fragColor = nitsToDestinationEncoding(tex * modulation);
}
