// SPDX-License-Identifier: Apache-2.0
#include "IlluminationRaster.h"
#include <array>
#include <cassert>
#include <iostream>
#include <vector>

int main() {
    std::array<bool, 256> levels{};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int value = tetris::udfps::bayer16(x, y);
            assert(value >= 0 && value < 256 && !levels[value]);
            levels[value] = true;
        }
    }
    constexpr int width = 64, height = 64, stride = 80, guard = 16;
    for (int sample = 0; sample <= 256; ++sample) {
        const float alpha = sample / 256.f;
        std::vector<uint8_t> allocation(2 * guard + stride * height * 4, 0xa5);
        uint8_t* pixels = allocation.data() + guard;
        tetris::udfps::fillIllumination(pixels, width, height, stride,
                                      48.f, 48.f, 8.f, 8.f, alpha);
        int sum = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < stride; ++x) {
                const uint8_t* p = pixels + (y * stride + x) * 4;
                if (x >= width) {
                    for (int c = 0; c < 4; ++c) assert(p[c] == 0xa5);
                    continue;
                }
                // Every sample must remain valid premultiplied RGBA.
                assert(p[0] == p[1] && p[1] == p[2] && p[2] <= p[3]);
                if (x < 16 && y < 16) {
                    assert(p[0] == 0 && p[1] == 0 && p[2] == 0);
                    sum += p[3];
                }
            }
        }
        assert(std::abs(sum / 256.f - alpha * 255.f) <= 1.f / 256.f);
        const uint8_t* center = pixels + (48 * stride + 48) * 4;
        for (int c = 0; c < 4; ++c) assert(center[c] == 255);
        for (int i = 0; i < guard; ++i) {
            assert(allocation[i] == 0xa5);
            assert(allocation[allocation.size() - 1 - i] == 0xa5);
        }
    }
    // Include short rows and heights on either side of the Bayer tile boundary.
    for (int smallWidth : {1, 3, 15, 16, 17, 31}) {
        for (int smallHeight : {1, 3, 15, 16, 17, 31}) {
            const int paddedStride = smallWidth + 7;
            std::vector<uint8_t> allocation(2 * guard + paddedStride * smallHeight * 4, 0xa5);
            uint8_t* pixels = allocation.data() + guard;
            tetris::udfps::fillIllumination(pixels, smallWidth, smallHeight, paddedStride,
                                          smallWidth / 2.f, smallHeight / 2.f, .25f, .25f, .8515625f);
            for (int y = 0; y < smallHeight; ++y) {
                for (int x = 0; x < paddedStride; ++x) {
                    const uint8_t* pixel = pixels + (y * paddedStride + x) * 4;
                    if (x >= smallWidth) {
                        for (int c = 0; c < 4; ++c) assert(pixel[c] == 0xa5);
                    } else {
                        assert(pixel[0] == pixel[1] && pixel[1] == pixel[2] && pixel[2] <= pixel[3]);
                    }
                }
            }
            for (int i = 0; i < guard; ++i) {
                assert(allocation[i] == 0xa5);
                assert(allocation[allocation.size() - 1 - i] == 0xa5);
            }
        }
    }
    std::cout << "PASS: all calibration steps, opaque sensor, premultiplied edges, Bayer boundaries and buffer bounds\n";
}
