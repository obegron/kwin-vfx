// SPDX-License-Identifier: GPL-2.0-or-later
// SDR, premultiplied encoded RGB input. Shared by KWin and the standalone GL test.
vec4 enhanceWindow(sampler2D image, vec2 uv, vec2 texel, float denoise, float sharpen)
{
    vec4 center = texture(image, uv);
    if ((denoise <= 0.0 && sharpen <= 0.0) || center.a <= 0.00001) {
        return center;
    }
    vec3 rgb = center.rgb / center.a;
    vec3 bilateral = vec3(0.0);
    vec3 spatial = vec3(0.0);
    vec3 lo = rgb;
    vec3 hi = rgb;
    float bilateralWeight = 0.0;
    float spatialWeight = 0.0;
    // A small spatial filter suppresses low-amplitude noise while rejecting edges.
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 tap = (x == 0 && y == 0) ? center : texture(image, uv + vec2(float(x), float(y)) * texel);
            vec3 sampleRgb = tap.rgb / max(tap.a, 0.00001);
            float alphaWeight = max(0.0, 1.0 - abs(tap.a - center.a) * 8.0);
            if (tap.a <= 0.00001) {
                alphaWeight = 0.0;
            }
            float weight = float((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1)) * alphaWeight;
            vec3 difference = sampleRgb - rgb;
            float rangeWeight = 1.0 / (1.0 + dot(difference, difference) / (0.06 * 0.06));
            bilateral += sampleRgb * weight * rangeWeight;
            bilateralWeight += weight * rangeWeight;
            spatial += sampleRgb * weight;
            spatialWeight += weight;
            if (alphaWeight > 0.5) {
                lo = min(lo, sampleRgb);
                hi = max(hi, sampleRgb);
            }
        }
    }
    vec3 smoothRgb = bilateral / max(bilateralWeight, 0.00001);
    vec3 detail = rgb - spatial / max(spatialWeight, 0.00001);
    // Suppress sharpening of low-amplitude noise. Bound to local range to avoid halos.
    float gate = smoothstep(0.015, 0.06, length(detail));
    vec3 result = mix(rgb, smoothRgb, denoise * 0.75) + detail * (sharpen * gate * 1.5);
    result = clamp(result, max(lo, vec3(0.0)), min(hi, vec3(1.0)));
    return vec4(result * center.a, center.a);
}
