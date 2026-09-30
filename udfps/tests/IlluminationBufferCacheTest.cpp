// SPDX-License-Identifier: Apache-2.0
#include "IlluminationBufferCache.h"
#include "IlluminationRaster.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

using tetris::udfps::IlluminationBufferCache;
using tetris::udfps::IlluminationBufferKey;
using Pixels = std::vector<uint8_t>;
using Buffer = std::shared_ptr<const Pixels>;

Buffer raster(const IlluminationBufferKey& key) {
    auto pixels = std::make_shared<Pixels>(key.width * key.height * 4);
    tetris::udfps::fillIllumination(pixels->data(), key.width, key.height, key.width,
                                  key.cx, key.cy, key.rx, key.ry, key.opacity);
    return pixels;
}

int main() {
    const IlluminationBufferKey a{64, 80, 32.f, 60.f, 8.f, 8.f, .34375f};
    auto b = a;
    b.opacity = .8515625f;
    auto c = a;
    c.opacity = .5f;
    IlluminationBufferCache<Buffer> cache;
    assert(cache.size() == 0 && !cache.find(a));
    cache.insert(a, {});
    assert(cache.size() == 0);
    const auto first = raster(a);
    const auto original = *first;
    cache.insert(a, first);
    assert(cache.find(a) == first && cache.size() == 1);
    // A duplicate key must keep the published pixels and not grow the cache.
    cache.insert(a, raster(a));
    assert(cache.find(a) == first && cache.size() == 1);

    // Every input affecting pixels must participate, with no approximate match.
    for (int field = 0; field < 7; ++field) {
        auto changed = a;
        switch (field) {
            case 0: ++changed.width; break;
            case 1: ++changed.height; break;
            case 2: changed.cx = std::nextafter(changed.cx, 100.f); break;
            case 3: changed.cy = std::nextafter(changed.cy, 100.f); break;
            case 4: changed.rx = std::nextafter(changed.rx, 100.f); break;
            case 5: changed.ry = std::nextafter(changed.ry, 100.f); break;
            case 6: changed.opacity = std::nextafter(changed.opacity, 1.f); break;
        }
        assert(!cache.find(changed));
    }
    cache.insert(b, raster(b));
    assert(cache.find(a) == first);
    cache.insert(c, raster(c));
    assert(cache.size() == 2 && !cache.find(b) && cache.find(a) == first);
    // Eviction and invalidation release only cache ownership. A consumer can
    // continue reading an earlier immutable buffer without pixel changes.
    cache.insert(b, raster(b));
    cache.clear();
    assert(cache.size() == 0 && !cache.find(a) && !cache.find(b) && !cache.find(c));
    assert(*first == original);

    // Alternate ambient/interactive rasters at every calibrated alpha, checking
    // the reused pixels against a fresh raster, including transformed geometry.
    int comparisons = 0;
    for (int rotation = 0; rotation < 4; ++rotation) {
        auto ambient = b;
        ambient.width = rotation % 2 ? 80 : 64;
        ambient.height = rotation % 2 ? 64 : 80;
        ambient.cx = ambient.width / 2.f;
        ambient.cy = ambient.height / 2.f;
        ambient.rx = rotation % 2 ? 7.5f : 8.25f;
        ambient.ry = rotation % 2 ? 8.25f : 7.5f;
        cache.clear();
        const auto ambientBuffer = raster(ambient);
        cache.insert(ambient, ambientBuffer);
        for (int alpha = 0; alpha <= 256; ++alpha) {
            auto interactive = ambient;
            interactive.opacity = alpha / 256.f;
            if (interactive == ambient) continue;
            const auto expected = raster(interactive);
            cache.insert(interactive, expected);
            assert(cache.find(ambient) == ambientBuffer);
            const auto actual = cache.find(interactive);
            assert(actual == expected && *actual == *raster(interactive));
            assert(*cache.find(ambient) == *raster(ambient));
            assert(cache.size() == 2);
            ++comparisons;
        }
    }
    std::cout << "PASS: exact keys, bounded LRU, immutable ownership and "
              << comparisons << " cached/fresh raster comparisons\n";
}
