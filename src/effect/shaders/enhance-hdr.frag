// SPDX-License-Identifier: GPL-2.0-or-later
#version 140
#include "colormanagement.glsl"
#include "saturation.glsl"
uniform sampler2D sampler;
uniform sampler2D aiSampler;
uniform vec4 modulation;
uniform float enhancementStrength;
uniform mat4 originalToDestination;
uniform vec2 originalTransferParams;
uniform float grainStrength;
uniform int grainFrame;
uniform vec2 texelSize;
in vec2 texcoord0;
out vec4 fragColor;

float grainRandom(uvec2 pixel, uint seed)
{
    uint value = pixel.x * 1973u + pixel.y * 9277u + seed * 26699u;
    value = (value ^ 61u) ^ (value >> 16u);
    value *= 9u;
    value ^= value >> 4u;
    value *= 668265261u;
    value ^= value >> 15u;
    return float(value & 65535u) / 65535.0;
}

void main()
{
    vec4 original = texture(sampler, texcoord0);
    if (original.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 originalStraight = original.rgb / max(original.a, 0.00001);
    // KWin's SDR capture uses gamma 2.2. Transform it independently so HDR
    // tone mapping cannot darken the original contribution near zero blend.
    vec3 originalNits = encodingToNits(vec4(originalStraight, 1.0), gamma22_EOTF,
        originalTransferParams.x, originalTransferParams.y).rgb;
    originalNits = clamp((originalToDestination * vec4(originalNits, 1.0)).rgb,
        vec3(0.0), vec3(maxDestinationLuminance));
    // RGB10A2 was converted to RGBA16F on the worker; its RGB is still PQ
    // encoded BT.2020. The SDK alpha is ignored in favor of original alpha.
    vec3 hdrNits = sourceEncodingToNitsInDestinationColorspace(vec4(texture(aiSampler, texcoord0).rgb, 1.0)).rgb;
    vec3 nits = mix(originalNits, hdrNits, enhancementStrength);
    if (grainStrength > 0.0) {
        uvec2 pixel = uvec2(floor(texcoord0 / texelSize));
        uint frame = uint(grainFrame);
        float noise = grainRandom(pixel, frame) + grainRandom(pixel, frame + 7919u) - 1.0;
        float luma = clamp(dot(originalStraight, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
        float amplitude = 0.08 * grainStrength * maxDestinationLuminance * (4.0 * luma * (1.0 - luma));
        nits = clamp(nits + vec3(noise * amplitude), vec3(0.0), vec3(maxDestinationLuminance));
    }
    vec4 color = adjustSaturation(vec4(nits * original.a, original.a));
    fragColor = nitsToDestinationEncoding(color * modulation);
}
