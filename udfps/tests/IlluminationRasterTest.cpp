// SPDX-License-Identifier: Apache-2.0
#include "IlluminationRaster.h"
#include <cassert>
#include <iostream>
#include <vector>

int main() {
    constexpr int width = 64, height = 64, stride = 80, guard = 16;
    for (int sample = 0; sample <= 256; ++sample) {
        const float alpha = sample / 256.f;
        std::vector<uint8_t> allocation(2 * guard + stride * height * 4, 0xa5);
        uint8_t* pixels = allocation.data() + guard;
        tetris::udfps::fillIllumination(pixels, width, height, stride,
                                      48.f, 48.f, 8.f, 8.f, alpha);
        // Golden MTK plane-alpha quantization for the exact n/256 calibration
        // steps: FCVTZS(alpha * 255) yields n-1 except at zero.
        const int expectedAlpha = sample == 0 ? 0 : sample - 1;
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
                    assert(p[3] == expectedAlpha);
                }
            }
        }
        const uint8_t* center = pixels + (48 * stride + 48) * 4;
        for (int c = 0; c < 4; ++c) assert(center[c] == 255);
        for (int i = 0; i < guard; ++i) {
            assert(allocation[i] == 0xa5);
            assert(allocation[allocation.size() - 1 - i] == 0xa5);
        }
    }
    // Include short rows, odd sizes and padded strides.
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
                        if (x == 0 && y == 0 && smallWidth > 3 && smallHeight > 3) {
                            assert(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0);
                            assert(pixel[3] == 217); // .8515625 -> floor(217.1484375)
                        }
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
    std::cout << "PASS: MTK plane-alpha quantization, opaque sensor, premultiplied edges and buffer bounds\n";
}
