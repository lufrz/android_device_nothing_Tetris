/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tetris::udfps {
// A 16x16 Bayer cell resolves the 1/256 calibration steps without a framework shader.
// Black RGB remains zero at every alpha, so the buffer is correctly premultiplied.
inline int bayer16(int x, int y) {
    static constexpr int matrix[2][2] = {{0, 2}, {3, 1}};
    int value = 0;
    for (int bit = 0; bit < 4; ++bit) {
        value = value * 4 + matrix[(y >> bit) & 1][(x >> bit) & 1];
    }
    return value;
}

// The caller validates dimensions, geometry and opacity; stride is measured in pixels.
template <typename Current>
inline bool fillIlluminationCancellable(uint8_t* pixels, int width, int height, uint32_t stride,
                                       float cx, float cy, float rx, float ry, float opacity,
                                       const Current& current) {
    if (!current()) return false;
    const float alphaByte = opacity * 255.0f;
    const int lowerAlpha = static_cast<int>(std::floor(alphaByte));
    const int highSamples = static_cast<int>(std::lround((alphaByte - lowerAlpha) * 256.0f));
    uint8_t alphaCell[16][16];
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            alphaCell[y][x] = std::min(255, lowerAlpha + (bayer16(x, y) < highSamples));
        }
    }
    // The background repeats every 16 pixels in both directions. Fill its first
    // 16 rows, then copy them without touching any padding at the end of a row.
    uint8_t background[16][16 * 4]{};
    for (int y = 0; y < std::min(height, 16); ++y) {
        for (int x = 0; x < 16; ++x) background[y][x * 4 + 3] = alphaCell[y][x];
        auto* row = pixels + static_cast<size_t>(y) * stride * 4;
        for (int x = 0; x < width; x += 16) {
            std::memcpy(row + x * 4, background[y], std::min(width - x, 16) * 4);
        }
    }
    for (int y = 16; y < height; ++y) {
        if ((y & 31) == 0 && !current()) return false;
        std::memcpy(pixels + static_cast<size_t>(y) * stride * 4,
                    pixels + static_cast<size_t>(y & 15) * stride * 4, width * 4);
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
            const int maskAlpha = alphaCell[y & 15][x & 15];
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
