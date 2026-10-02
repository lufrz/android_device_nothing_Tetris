/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tetris::udfps {
// The caller validates dimensions, geometry and opacity; stride is measured in pixels.
template <typename Current>
inline bool fillIlluminationCancellable(uint8_t* pixels, int width, int height, uint32_t stride,
                                       float cx, float cy, float rx, float ry, float opacity,
                                       const Current& current) {
    if (!current()) return false;
    // MTK's composer truncates plane alpha to an 8-bit value. Match that
    // conversion: dithering its fractional part makes compensation darker.
    // Black RGB stays zero, so the buffer remains correctly premultiplied.
    const uint8_t maskAlpha = static_cast<uint8_t>(opacity * 255.0f);
    for (int x = 0; x < width; ++x) {
        pixels[x * 4] = pixels[x * 4 + 1] = pixels[x * 4 + 2] = 0;
        pixels[x * 4 + 3] = maskAlpha;
    }
    // Copy only active pixels, preserving padding at the end of each row.
    for (int y = 1; y < height; ++y) {
        if ((y & 31) == 0 && !current()) return false;
        std::memcpy(pixels + static_cast<size_t>(y) * stride * 4,
                    pixels, width * 4);
    }

    // Keep the existing coverage and rounding at the sensor edge. Only this
    // small rectangle needs floating-point work; the rest is already black.
    const int left = std::max(0, static_cast<int>(std::floor(cx - rx - 2.f)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - ry - 2.f)));
    const int right = std::min(width, static_cast<int>(std::ceil(cx + rx + 2.f)));
    const int bottom = std::min(height, static_cast<int>(std::ceil(cy + ry + 2.f)));
    const float edge = 1.0f / std::min(rx, ry);
    for (int y = top; y < bottom; ++y) {
        if (((y - top) & 31) == 0 && !current()) return false;
        auto* row = pixels + static_cast<size_t>(y) * stride * 4;
        for (int x = left; x < right; ++x) {
            float coverage = 0;
            if (std::abs(x + 0.5f - cx) <= rx + 1 && std::abs(y + 0.5f - cy) <= ry + 1) {
                const float dx = (x + 0.5f - cx) / rx;
                const float dy = (y + 0.5f - cy) / ry;
                coverage = std::clamp((1.0f - std::sqrt(dx * dx + dy * dy)) / edge + 0.5f,
                                      0.0f, 1.0f);
            }
            const uint8_t white = static_cast<uint8_t>(std::lround(coverage * 255.0f));
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = white;
            row[x * 4 + 3] = static_cast<uint8_t>(
                    std::clamp(std::lround(white + (1.0f - coverage) * maskAlpha), 0L, 255L));
        }
    }
    return current();
}

inline void fillIllumination(uint8_t* pixels, int width, int height, uint32_t stride,
                             float cx, float cy, float rx, float ry, float opacity) {
    fillIlluminationCancellable(pixels, width, height, stride, cx, cy, rx, ry, opacity,
                                [] { return true; });
}
} // namespace tetris::udfps
