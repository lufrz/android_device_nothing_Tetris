// SPDX-License-Identifier: Apache-2.0
#include "NativeCacheSurfaceStubs.h"
#include "IlluminationSurface.cpp"
#include <iostream>
#include <limits>

namespace {
unsigned testCases = 0;
void reset() {
    gBufferCache.clear();
    gBuffer.clear();
    gSurface.clear();
    gClient.clear();
    gLastPresent.reset();
    android::compositorBuffers.clear();
    android::submittedSurfaces.clear();
    native_test::state = {};
    Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeSetGeneration(nullptr, nullptr, 1);
}
bool request(float alpha = .3359375f, int stack = 0) {
    return Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeShow(
            nullptr, nullptr, 32, 48, stack, 16.f, 24.f, 6.f, 6.f, alpha,
            gGeneration.load());
}
void cancel() {
    Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeSetGeneration(
            nullptr, nullptr, gGeneration.load() + 1);
}
void reason(const std::string& expected) {
    assert(gLastPresent != nullptr && gLastPresent->result == expected);
}
void assertRaster(const android::sp<android::GraphicBuffer>& buffer, float alpha) {
    std::vector<uint8_t> reference(buffer->bytes.size(), 0xa5);
    tetris::udfps::fillIllumination(reference.data(), 32, 48, buffer->stride,
                                  16.f, 24.f, 6.f, 6.f, alpha);
    assert(buffer->bytes == reference);
}
void hitAndMiss() {
    reset();
    assert(request());
    auto& s = native_test::state;
    auto first = gBuffer;
    const auto originalPixels = first->bytes;
    assertRaster(first, .3359375f);
    assert(s.allocations == 1 && s.locks == 1 && s.unlocks == 1 && s.markerReads == 1);
    assert(s.shows == 1 && s.showCallbacks == 1 && s.showWaits == 1);
    assert(!gLastPresent->bufferCacheHit && gBufferCache.size() == 1);
    ++testCases;

    // Reusing pixels must still create a fresh surface/transaction and await its fence.
    assert(request(.3359375f, 9));
    assert(gBuffer == first && first->bytes == originalPixels);
    assert(s.allocations == 1 && s.locks == 1 && s.unlocks == 1 && s.markerReads == 1);
    assert(s.surfaces == 2 && s.shows == 2 && s.bufferSubmits == 2);
    assert(s.commits == 2 && s.showCallbacks == 2 && s.showWaits == 2);
    assert(s.hides == 1 && s.hideWaits == 1 && s.detachedBeforeHide);
    assert(s.lastLayerStack == 9 && android::submittedSurfaces[0] != android::submittedSurfaces[1]);
    assert(gLastPresent->bufferCacheHit && gLastPresent->rasterMs == 0);
    reason("presented");
    ++testCases;

    assert(request(.8515625f));
    auto ambient = gBuffer;
    assert(ambient != first && s.allocations == 2 && s.locks == 2);
    assertRaster(ambient, .8515625f);
    assert(first->bytes == originalPixels && gBufferCache.size() == 2);
    assert(request(.3359375f) && gBuffer == first && s.allocations == 2);
    assert(request(.5f) && s.allocations == 3 && gBufferCache.size() == 2);
    // A was the last hit: the evicted entry is B, whose compositor reference stays immutable.
    const auto ambientPixels = ambient->bytes;
    assert(request(.8515625f) && s.allocations == 4 && gBuffer != ambient);
    assert(ambient->bytes == ambientPixels && first->bytes == originalPixels);
    ++testCases;
}
void preparationFailures() {
    for (int warm = 0; warm < 2; ++warm) for (int fault = 0; fault < 6; ++fault) {
        reset();
        if (warm) assert(request());
        const auto previous = gBuffer;
        const auto previousPixels = warm ? previous->bytes : std::vector<uint8_t>{};
        const float alpha = warm ? .8515625f : .3359375f;
        auto& s = native_test::state;
        switch (fault) {
            case 0: s.allocationError = -ENOMEM; break;
            case 1: s.markerError = -EIO; break;
            case 2: s.badMarker = true; break;
            case 3: s.lockError = -EIO; break;
            case 4: s.nullAddress = true; break;
            case 5: s.unlockError = -EIO; break;
        }
        assert(!request(alpha) && gBufferCache.size() == static_cast<size_t>(warm));
        assert(s.shows == warm && gBuffer == nullptr);
        assert(gBufferCache.find({32, 48, 16.f, 24.f, 6.f, 6.f, alpha}) == nullptr);
        if (warm) {
            assert(gBufferCache.find({32, 48, 16.f, 24.f, 6.f, 6.f, .3359375f}) == previous);
            assert(previous->bytes == previousPixels);
        }
        s.allocationError = s.markerError = s.lockError = s.unlockError = 0;
        s.badMarker = s.nullAddress = false;
        assert(request(alpha) && s.allocations == warm + 2 && !gLastPresent->bufferCacheHit);
        assertRaster(gBuffer, alpha);
        ++testCases;
    }
}
void hitsStillRequirePresentation() {
    for (int fault = 0; fault < 5; ++fault) {
        reset();
        assert(request());
        auto& s = native_test::state;
        switch (fault) {
            case 0: s.showLatch = false; break;
            case 1: s.wrongSurface = true; break;
            case 2: s.showFenceValid = false; break;
            case 3: s.showNullFence = true; break;
            case 4: s.showFenceError = -EIO; break;
        }
        assert(!request() && gLastPresent->bufferCacheHit);
        assert(s.allocations == 1 && s.locks == 1 && s.shows == 2 && s.showCallbacks == 2);
        if (fault < 2) reason("surface_not_latched");
        else if (fault < 4) reason("invalid_present_fence");
        else { reason("present_fence_error:-5"); assert(s.showWaits == 2); }
        assert(gSurface == nullptr && gBuffer == nullptr);
        ++testCases;
    }
    reset();
    assert(request());
    native_test::state.showCallback = false;
    native_test::state.onApply = cancel;
    assert(!request() && gLastPresent->bufferCacheHit);
    reason("cancelled_waiting_callback");
    assert(native_test::state.showCallbacks == 1 && native_test::state.showWaits == 1);
    ++testCases;
}
void hideFailuresAndDeath() {
    reset();
    assert(request());
    auto surface = gSurface;
    auto buffer = gBuffer;
    native_test::state.hideError = -EIO;
    assert(!hide() && gBufferCache.size() == 0);
    assert(gSurface == surface && gBuffer == buffer);
    const auto pixels = buffer->bytes;
    native_test::state.hideError = 0;
    assert(hide() && gSurface == nullptr && gBuffer == nullptr);
    assert(buffer->bytes == pixels);
    assert(request() && native_test::state.allocations == 2);
    ++testCases;

    for (int fault = 0; fault < 3; ++fault) {
        reset();
        assert(request());
        if (fault == 0) native_test::state.hideFenceValid = false;
        if (fault == 1) native_test::state.hideFenceError = -EIO;
        if (fault == 2) native_test::state.hideError = android::DEAD_OBJECT;
        assert(!hide() && gBufferCache.size() == 0);
        assert(gSurface == nullptr && gBuffer == nullptr);
        if (fault == 2) assert(gClient == nullptr);
        ++testCases;
    }
    reset();
    assert(request());
    native_test::state.showError = android::DEAD_OBJECT;
    assert(!request() && gLastPresent->bufferCacheHit);
    assert(gBufferCache.size() == 0 && gClient == nullptr);
    native_test::state.showError = 0;
    assert(request() && native_test::state.allocations == 2 && native_test::state.clients == 2);
    ++testCases;
}
void cancellationAndInvalidInputs() {
    reset();
    assert(!show(32, 48, 0, 16.f, 24.f, 6.f, 6.f, .5f, 0));
    reason("cancelled_before_prepare");
    assert(!native_test::state.shows && !native_test::state.allocations);
    ++testCases;
    for (int phase = 0; phase < 3; ++phase) {
        reset();
        if (phase == 0) native_test::state.onUnlock = cancel;
        if (phase == 1) native_test::state.onCreate = cancel;
        if (phase == 2) { assert(request()); native_test::state.onCreate = cancel; }
        const int oldShows = native_test::state.shows;
        assert(!request());
        assert(native_test::state.shows == oldShows);
        reason(phase == 0 ? "cancelled_after_prepare" : "cancelled_before_submit");
        if (phase == 0) assert(gBufferCache.size() == 0);
        assert(gSurface == nullptr && gBuffer == nullptr);
        ++testCases;
    }
    reset();
    assert(request());
    native_test::state.onShowFence = cancel;
    assert(!request() && gLastPresent->bufferCacheHit);
    reason("cancelled_waiting_fence");
    assert(native_test::state.showWaits == 2);
    ++testCases;

    for (int fault = 0; fault < 4; ++fault) {
        reset();
        if (fault == 0) native_test::state.clientError = -EIO;
        if (fault == 1) native_test::state.noSurface = true;
        if (fault == 2) native_test::state.invalidSurface = true;
        if (fault < 3) assert(!request());
        else assert(!show(32, 48, 0, 16.f, 24.f, 6.f, 6.f,
                          std::numeric_limits<float>::quiet_NaN(), 1));
        assert(gBufferCache.size() == 0 && native_test::state.shows == 0);
        ++testCases;
    }
}
}
int main() {
    hitAndMiss();
    preparationFailures();
    hitsStillRequirePresentation();
    hideFailuresAndDeath();
    cancellationAndInvalidInputs();
    reset();
    std::cout << "PASS actual IlluminationSurface.cpp native cache cases=" << testCases
              << "; immutable reuse, real raster, fresh transaction/latch/fence, failures and cancellation\n";
}
