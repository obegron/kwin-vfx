// SPDX-License-Identifier: GPL-2.0-or-later
#version 140
#include "colormanagement.glsl"
#include "saturation.glsl"
#include "kwin-vfx/filter.glsl"
#include "kwin-vfx/cas.glsl"

uniform sampler2D sampler;
uniform vec4 modulation;
uniform vec2 texelSize;
uniform float denoiseStrength;
uniform float sharpenStrength;
uniform int filterMode; // 0: spatial shader, 1: original while pending, 2: AI+CAS, 3: CAS only
uniform sampler2D aiSampler;
uniform float enhancementStrength;
uniform float aiSharpenStrength;
uniform vec2 aiTexelSize;
uniform float grainStrength;
uniform int grainFrame;
in vec2 texcoord0;
out vec4 fragColor;

vec4 sampleAi()
{
    return casSharpen(aiSampler, texcoord0, aiTexelSize, aiSharpenStrength);
}

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

vec4 addFilmGrain(vec4 color)
{
    if (grainStrength <= 0.0 || color.a <= 0.0) return color;
    vec3 straight = clamp(color.rgb / color.a, 0.0, 1.0);
    float luma = dot(straight, vec3(0.2126, 0.7152, 0.0722));
    // Monochrome triangular noise, strongest in midtones. Pure black/white
    // stay unchanged, preserving letterbox bars; alpha is never modified.
    uvec2 pixel = uvec2(floor(texcoord0 / texelSize));
    uint frame = uint(grainFrame);
    float noise = grainRandom(pixel, frame) + grainRandom(pixel, frame + 7919u) - 1.0;
    float amplitude = 0.08 * grainStrength * (4.0 * luma * (1.0 - luma));
    color.rgb = clamp(straight + vec3(noise * amplitude), 0.0, 1.0) * color.a;
    return color;
}

void main()
{
    vec4 tex = texture(sampler, texcoord0);
    if (filterMode == 0) {
        tex = enhanceWindow(sampler, texcoord0, texelSize, denoiseStrength, sharpenStrength);
    } else if (filterMode == 3) {
        tex = casSharpen(sampler, texcoord0, texelSize, aiSharpenStrength * enhancementStrength);
    } else if (filterMode == 2) {
        vec4 ai = sampleAi();
        if (ai.a > 0.99) {
            tex.rgb = mix(tex.rgb, ai.rgb * tex.a, enhancementStrength);
        }
    }
    tex = addFilmGrain(tex);
    tex = sourceEncodingToNitsInDestinationColorspace(tex);
    tex = adjustSaturation(tex);
    fragColor = nitsToDestinationEncoding(tex * modulation);
}
