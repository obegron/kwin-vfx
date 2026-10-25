// SPDX-License-Identifier: MIT
// Sharpen-only GLSL port of AMD FidelityFX CAS, CAS_BETTER_DIAGONALS + CAS_SLOW.
// https://github.com/GPUOpen-Effects/FidelityFX-CAS/blob/master/ffx-cas/ffx_cas.h
// Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

vec3 casToLinear(vec3 value)
{
    // KWin 6.8's display-referred ColorDescription::sRGB uses gamma 2.2.
    return pow(clamp(value, 0.0, 1.0), vec3(2.2));
}

vec3 casFromLinear(vec3 value)
{
    return pow(clamp(value, 0.0, 1.0), vec3(1.0 / 2.2));
}

vec4 casSharpen(sampler2D image, vec2 uv, vec2 texel, float strength)
{
    vec4 center = texture(image, uv);
    if (strength <= 0.0 || center.a < 0.99) return center;
    vec4 taps[9];
    int index = 0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            taps[index] = texture(image, uv + vec2(float(x), float(y)) * texel);
            // Avoid pulling shadow/transparent RGB into the opaque surface.
            if (taps[index].a < 0.99) return center;
            ++index;
        }
    }
    vec3 a = casToLinear(taps[0].rgb / taps[0].a), b = casToLinear(taps[1].rgb / taps[1].a);
    vec3 c = casToLinear(taps[2].rgb / taps[2].a), d = casToLinear(taps[3].rgb / taps[3].a);
    vec3 e = casToLinear(center.rgb / center.a), f = casToLinear(taps[5].rgb / taps[5].a);
    vec3 g = casToLinear(taps[6].rgb / taps[6].a), h = casToLinear(taps[7].rgb / taps[7].a);
    vec3 i = casToLinear(taps[8].rgb / taps[8].a);
    vec3 minimum = min(min(min(d, e), f), min(b, h));
    vec3 maximum = max(max(max(d, e), f), max(b, h));
    minimum += min(minimum, min(min(a, c), min(g, i)));
    maximum += max(maximum, max(max(a, c), max(g, i)));
    vec3 amplitude = sqrt(clamp(min(minimum, vec3(2.0) - maximum) / max(maximum, vec3(0.000001)), 0.0, 1.0));
    vec3 weight = amplitude * (-1.0 / mix(8.0, 5.0, clamp(strength, 0.0, 1.0)));
    vec3 result = clamp(((b + d + f + h) * weight + e) / (vec3(1.0) + 4.0 * weight), 0.0, 1.0);
    // UI zero means exact bypass; fade the CAS result in continuously.
    vec3 encoded = mix(center.rgb / center.a, casFromLinear(result), strength);
    return vec4(encoded * center.a, center.a);
}
