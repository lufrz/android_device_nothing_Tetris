/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <array>
#include <cstddef>
#include <utility>

namespace tetris::udfps {

struct IlluminationBufferKey {
    int width, height;
    float cx, cy, rx, ry, opacity;

    bool operator==(const IlluminationBufferKey& other) const {
        // No rounding: even a small change at the sensor edge or in the
        // compensation must produce new pixels. Inputs are validated by show().
        return width == other.width && height == other.height && cx == other.cx
                && cy == other.cy && rx == other.rx && ry == other.ry
                && opacity == other.opacity;
    }
};

// Worker-thread only. Published buffers are immutable, including after eviction:
// a compositor may still own a reference after a detach or a cancelled scan.
// Keep at most two rasters so ambient and interactive scans can alternate.
template <typename Buffer>
class IlluminationBufferCache {
public:
    Buffer find(const IlluminationBufferKey& key) {
        for (size_t i = 0; i < mSize; ++i) {
            if (mEntries[i].key == key) {
                if (i != 0) std::swap(mEntries[0], mEntries[i]);
                return mEntries[0].buffer;
            }
        }
        return {};
    }

    // Only call after allocation, composer-marker verification, raster and
    // unlock succeed. A miss always gets a new buffer, never an in-place refill.
    void insert(const IlluminationBufferKey& key, const Buffer& buffer) {
        if (buffer == nullptr) return;
        if (find(key) != nullptr) return;
        mEntries[1] = std::move(mEntries[0]);
        mEntries[0] = {key, buffer};
        if (mSize < mEntries.size()) ++mSize;
    }

    void clear() {
        mEntries = {};
        mSize = 0;
    }

    size_t size() const { return mSize; }

private:
    struct Entry {
        IlluminationBufferKey key{};
        Buffer buffer{};
    };
    std::array<Entry, 2> mEntries{};
    size_t mSize = 0;
};
} // namespace tetris::udfps
