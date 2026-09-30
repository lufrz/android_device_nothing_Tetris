// SPDX-License-Identifier: Apache-2.0
#include "NativeCacheSurfaceStubs.h"
#include "IlluminationSurface.cpp"
#include <iostream>
#include <limits>
#include <thread>

namespace {
unsigned testCases = 0;
void reset() {
    native_test::state.onDestroy = {};
    cancelPreparation(true);
    gLastPreparation = {};
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
jlong preparationToken() {
    return Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeCancelPreparation(nullptr, nullptr);
}
bool prepare(jlong token, float alpha = .3359375f) {
    return Java_org_lineageos_tetris_udfps_IlluminationApplication_nativePrepareBuffer(
            nullptr, nullptr, 32, 48, 16.f, 24.f, 6.f, 6.f, alpha, token);
}
bool has(float alpha = .3359375f) {
    return Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeHasBuffer(
            nullptr, nullptr, 32, 48, 16.f, 24.f, 6.f, 6.f, alpha);
}
struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
    void pause() {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
    }
    void await() {
        std::unique_lock lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
};
void preparedBuffers() {
    reset();
    bool result = false;
    const auto token = preparationToken();
    std::thread producer([&] { result = prepare(token); });
    producer.join();
    auto& s = native_test::state;
    assert(result && has() && gBufferCache.size() == 0);
    assert(s.allocations == 1 && s.locks == 1 && s.unlocks == 1 && s.markerReads == 1);
    assert(s.clients == 0 && s.surfaces == 0 && s.shows == 0 && s.hides == 0);
    auto buffer = gPreparedBuffer.buffer;
    assertRaster(buffer, .3359375f);
    assert(preparationToken() > token && has());
    cancel(); // Binder generation changes keep completed pixels but cancel unfinished work.
    assert(has() && request(.3359375f, 7));
    assert(gBuffer == buffer && gLastPresent->bufferPrepared && !gLastPresent->bufferCacheHit);
    assert(gPreparedBuffer.buffer == nullptr && gBufferCache.size() == 1);
    assert(s.allocations == 1 && s.shows == 1 && s.showCallbacks == 1 && s.showWaits == 1);
    assert(diagnostics().find("buffer_origin=prepared") != std::string::npos);
    assert(request() && gLastPresent->bufferCacheHit && s.allocations == 1);
    ++testCases;

    // Every key component is exact; a completed wrong-key result is not consumed.
    reset();
    assert(prepare(preparationToken()));
    const BufferKey original{32, 48, 16.f, 24.f, 6.f, 6.f, .3359375f};
    for (int component = 0; component < 7; ++component) {
        auto key = original;
        switch (component) {
            case 0: ++key.width; break;
            case 1: ++key.height; break;
            case 2: key.cx += .25f; break;
            case 3: key.cy += .25f; break;
            case 4: key.rx += .25f; break;
            case 5: key.ry += .25f; break;
            case 6: key.opacity = std::nextafter(key.opacity, 1.f); break;
        }
        assert(takePreparedBuffer(key).buffer == nullptr);
        assert(!Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeHasBuffer(
                nullptr, nullptr, key.width, key.height, key.cx, key.cy, key.rx, key.ry, key.opacity));
        assert(has());
    }
    assert(request(.5f) && !gLastPresent->bufferPrepared && has());
    assert(request() && gLastPresent->bufferPrepared && s.allocations == 2);
    ++testCases;

    for (int fault = 0; fault < 6; ++fault) {
        reset();
        assert(prepare(preparationToken()));
        const auto retained = gPreparedBuffer.buffer;
        switch (fault) {
            case 0: s.allocationError = -ENOMEM; break;
            case 1: s.markerError = -EIO; break;
            case 2: s.badMarker = true; break;
            case 3: s.lockError = -EIO; break;
            case 4: s.nullAddress = true; break;
            case 5: s.unlockError = -EIO; break;
        }
        assert(!prepare(preparationToken(), .5f));
        assert(gPreparedBuffer.buffer == retained && has() && !has(.5f));
        assert(gBufferCache.size() == 0 && s.clients == 0 && s.shows == 0);
        ++testCases;
    }
    reset();
    assert(!prepare(0) && !prepare(-1));
    auto stale = preparationToken();
    preparationToken();
    assert(!prepare(stale));
    assert(!Java_org_lineageos_tetris_udfps_IlluminationApplication_nativePrepareBuffer(
            nullptr, nullptr, 32, 48, 16.f, 24.f, 40.f, 6.f, .5f, preparationToken()));
    assert(!prepare(preparationToken(), std::numeric_limits<float>::quiet_NaN()));
    assert(s.allocations == 0 && !has());
    ++testCases;

    // Prepared pixels must obey exactly the same optical readiness checks as a miss.
    for (int fault = 0; fault < 3; ++fault) {
        reset();
        assert(prepare(preparationToken()));
        if (fault == 0) s.showLatch = false;
        if (fault == 1) s.showFenceValid = false;
        if (fault == 2) s.showFenceError = -EIO;
        assert(!request() && gLastPresent->bufferPrepared && s.shows == 1);
        assert(s.allocations == 1 && s.showCallbacks == 1);
        ++testCases;
    }
}
void producerCancellation() {
    for (int phase = 0; phase < 3; ++phase) {
        reset();
        Gate gate;
        auto& s = native_test::state;
        if (phase < 2) s.onLock = [&] { gate.pause(); };
        else s.onUnlock = [&] { gate.pause(); };
        const auto token = preparationToken();
        bool result = true;
        std::thread producer([&] { result = prepare(token); });
        gate.await();
        assert(!has()); // No partial buffer is exposed while gralloc/raster work is running.
        if (phase == 0) preparationToken();
        else cancel();
        gate.release();
        producer.join();
        assert(!result && !has() && gBufferCache.size() == 0);
        assert(s.unlocks == 1 && s.clients == 0 && s.shows == 0);
        assert(std::string(gLastPreparation.result).find("cancelled") != std::string::npos);
        ++testCases;
    }
    // Hide invalidation while the producer is blocked must prevent later resurrection.
    reset();
    assert(request());
    assert(prepare(preparationToken(), .5f));
    Gate gate;
    native_test::state.onLock = [&] { gate.pause(); };
    native_test::state.hideError = -EIO;
    const auto token = preparationToken();
    bool result = true;
    std::thread producer([&] { result = prepare(token, .75f); });
    gate.await();
    assert(!hide() && !has(.5f));
    gate.release();
    producer.join();
    assert(!result && !has(.75f) && gBufferCache.size() == 0);
    ++testCases;

    reset();
    assert(request());
    assert(prepare(preparationToken(), .5f));
    native_test::state.showError = android::DEAD_OBJECT;
    assert(!request() && !has(.5f) && gPreparedBuffer.buffer == nullptr);
    ++testCases;

    // GraphicBuffer destruction may perform Binder I/O. Both replacement and clear
    // must release the slot mutex before dropping their last buffer reference.
    reset();
    unsigned destroyed = 0;
    native_test::state.onDestroy = [&] {
        bool unlocked = false;
        std::thread probe([&] {
            unlocked = gPreparationMutex.try_lock();
            if (unlocked) gPreparationMutex.unlock();
        });
        probe.join();
        assert(unlocked);
        ++destroyed;
    };
    assert(prepare(preparationToken()));
    assert(prepare(preparationToken(), .5f) && destroyed == 1);
    cancelPreparation(true);
    assert(destroyed == 2 && !has());
    native_test::state.onDestroy = {};
    ++testCases;
}
void cancellableRaster() {
    const int width = 40, height = 192, stride = 43;
    std::vector<uint8_t> reference(stride * height * 4, 0xa5);
    tetris::udfps::fillIllumination(reference.data(), width, height, stride, 20, 96, 18, 90, .3359375f);
    unsigned totalChecks = 0;
    auto complete = reference;
    assert(tetris::udfps::fillIlluminationCancellable(complete.data(), width, height, stride,
            20, 96, 18, 90, .3359375f, [&] { ++totalChecks; return true; }));
    assert(complete == reference && totalChecks >= 12);
    // Stop at each real checkpoint, including sensor rows, and never report a partial fill complete.
    for (unsigned stop = 1; stop <= totalChecks; ++stop) {
        std::vector<uint8_t> pixels(stride * height * 4, 0xa5);
        unsigned calls = 0;
        assert(!tetris::udfps::fillIlluminationCancellable(pixels.data(), width, height, stride,
                20, 96, 18, 90, .3359375f, [&] { return ++calls < stop; }));
        assert(calls == stop);
        for (int y = 0; y < height; ++y) for (int x = width * 4; x < stride * 4; ++x)
            assert(pixels[y * stride * 4 + x] == 0xa5);
    }
    ++testCases;
}

}
int main() {
    hitAndMiss();
    preparationFailures();
    hitsStillRequirePresentation();
    hideFailuresAndDeath();
    cancellationAndInvalidInputs();
    preparedBuffers();
    producerCancellation();
    cancellableRaster();
    reset();
    std::cout << "PASS actual IlluminationSurface.cpp native cache cases=" << testCases
              << "; immutable reuse, real raster, fresh transaction/latch/fence, producer publication, cancellation and failures\n";
}
