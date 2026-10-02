/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "TetrisUdfpsSurface"

#include "IlluminationBufferCache.h"
#include "IlluminationRaster.h"

#include <jni.h>
#include <binder/ProcessState.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <log/log.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicTypes.h>
#include <ui/PixelFormat.h>
#include <utils/String8.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace {
using namespace android;
using Clock = std::chrono::steady_clock;

// Rendering entry points run on the Java worker. Callbacks and diagnostic reads use locks.
sp<SurfaceComposerClient> gClient;
sp<SurfaceControl> gSurface;
sp<GraphicBuffer> gBuffer;
tetris::udfps::IlluminationBufferCache<sp<GraphicBuffer>> gBufferCache;
std::atomic<int64_t> gGeneration{0};

// The background producer never touches the compositor or the worker-owned cache.
// Only a complete immutable buffer crosses this lock. Buffer destruction stays
// outside it because gralloc/free and libgui cache callbacks can perform Binder I/O.
using BufferKey = tetris::udfps::IlluminationBufferKey;
struct PreparedBuffer {
    BufferKey key{};
    sp<GraphicBuffer> buffer;
};
struct PreparationState {
    uint64_t token = 0;
    const char* result = "none";
};
std::mutex gPreparationMutex;
std::atomic<uint64_t> gPreparationEpoch{0};
PreparedBuffer gPreparedBuffer;
PreparationState gLastPreparation;

uint64_t cancelPreparation(bool clearCompleted = false) {
    PreparedBuffer discarded;
    uint64_t token;
    {
        std::lock_guard lock(gPreparationMutex);
        token = gPreparationEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
        if (clearCompleted) std::swap(discarded, gPreparedBuffer);
    }
    return token;
}

bool validKey(const BufferKey& key) {
    return key.width > 0 && key.height > 0 && key.width <= 4096 && key.height <= 4096
            && std::isfinite(key.opacity) && key.opacity >= 0 && key.opacity <= 1
            && std::isfinite(key.cx) && std::isfinite(key.cy) && std::isfinite(key.rx)
            && std::isfinite(key.ry) && key.rx > 0 && key.ry > 0
            && key.cx - key.rx >= 0 && key.cy - key.ry >= 0
            && key.cx + key.rx <= key.width && key.cy + key.ry <= key.height;
}

PreparedBuffer takePreparedBuffer(const BufferKey& key) {
    PreparedBuffer ready;
    {
        std::lock_guard lock(gPreparationMutex);
        if (gPreparedBuffer.buffer != nullptr && gPreparedBuffer.key == key) {
            std::swap(ready, gPreparedBuffer);
        }
    }
    return ready;
}

struct PresentState {
    std::mutex mutex;
    std::condition_variable condition;
    int64_t generation = 0;
    bool bufferCacheHit = false;
    bool bufferPrepared = false;
    size_t bufferCacheEntries = 0;
    std::string result = "preparing";
    bool completed = false;
    bool latched = false;
    sp<Fence> fence;
};

std::mutex gDiagnosticsMutex;
std::shared_ptr<PresentState> gLastPresent;
struct HideState {
    status_t status = NO_ERROR;
    std::string result = "none";
};
HideState gLastHide;

bool prepareBuffer(const BufferKey& key, uint64_t token) {
    const auto current = [token] {
        return token != 0 && gPreparationEpoch.load(std::memory_order_relaxed) == token;
    };
    const auto finish = [&](const char* result) {
        std::lock_guard lock(gPreparationMutex);
        gLastPreparation = {token, result};
        return false;
    };
    if (!validKey(key)) return finish("invalid_geometry");
    if (!current()) return finish("cancelled_before_prepare");
    auto buffer = sp<GraphicBuffer>::make(key.width, key.height, PIXEL_FORMAT_RGBA_8888, 1,
            GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_TEXTURE,
            "NTFingerprintDimLayer");
    if (buffer->initCheck() != NO_ERROR) return finish("buffer_allocation_failed");
    if (!current()) return finish("cancelled_after_allocation");
    std::string bufferName;
    if (GraphicBufferMapper::get().getName(buffer->handle, &bufferName) != NO_ERROR
            || bufferName.find("NTFingerprintDimLayer") == std::string::npos) {
        return finish("missing_composer_buffer_marker");
    }
    void* address = nullptr;
    const status_t locked = buffer->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &address);
    if (locked != NO_ERROR) return finish("buffer_lock_failed");
    if (address == nullptr) {
        buffer->unlock();
        return finish("buffer_lock_failed");
    }
    const bool complete = tetris::udfps::fillIlluminationCancellable(
            static_cast<uint8_t*>(address), key.width, key.height, buffer->getStride(),
            key.cx, key.cy, key.rx, key.ry, key.opacity, current);
    const status_t unlocked = buffer->unlock();
    if (unlocked != NO_ERROR) return finish("buffer_unlock_failed");
    if (!complete || !current()) return finish("cancelled_during_prepare");
    PreparedBuffer ready{key, buffer};
    {
        std::lock_guard lock(gPreparationMutex);
        // Serialize the final epoch check and publication with cancellation and
        // invalidation. An older producer cannot republish after a failed hide.
        if (!current()) {
            gLastPreparation = {token, "cancelled_before_publish"};
            return false;
        }
        std::swap(ready, gPreparedBuffer);
        gLastPreparation = {token, "prepared"};
    }
    return true;
}

std::string diagnostics() {
    std::shared_ptr<PresentState> pending;
    HideState lastHide;
    {
        std::lock_guard lock(gDiagnosticsMutex);
        pending = gLastPresent;
        lastHide = gLastHide;
    }
    std::ostringstream out;
    if (pending != nullptr) {
        std::lock_guard lock(pending->mutex);
        out << "generation=" << pending->generation << " result=" << pending->result
            << " buffer_cache=" << (pending->bufferCacheHit ? "hit" : "miss")
            << " buffer_origin=" << (pending->bufferCacheHit ? "cache"
                    : pending->bufferPrepared ? "prepared" : "rendered")
            << " cache_entries=" << pending->bufferCacheEntries;
    } else {
        out << "presentation=none";
    }
    out << " hide={status=" << lastHide.status << " result=" << lastHide.result << "}";
    PreparationState preparation;
    bool ready;
    {
        std::lock_guard lock(gPreparationMutex);
        preparation = gLastPreparation;
        ready = gPreparedBuffer.buffer != nullptr;
    }
    out << " preparation={token=" << preparation.token << " result=" << preparation.result
        << " ready=" << ready << "}";
    return out.str();
}

// HBM follows the named buffer through the vendor composer. Wait for its removal
// before acknowledging cleanup; never lower HBM while its compensation is still visible.
bool hide() {
    if (gSurface == nullptr) {
        gBuffer.clear();
        return true;
    }
    const auto started = Clock::now();
    const auto pending = std::make_shared<PresentState>();
    const auto target = gSurface;
    auto callback = [pending](void*, nsecs_t, const sp<Fence>& fence,
                              const std::vector<SurfaceControlStats>&) {
        std::lock_guard lock(pending->mutex);
        pending->fence = fence;
        pending->completed = true;
        pending->condition.notify_all();
    };
    // A hide-only transaction has no buffer latch and normally carries no present
    // fence. Detach the buffer in this transaction so SurfaceFlinger reports the
    // frame which removes the compensation and the composer's HBM marker.
    const status_t status = SurfaceComposerClient::Transaction().setBuffer(target, nullptr)
            .hide(target)
            .addTransactionCompletedCallback(std::move(callback), nullptr).apply();
    // libgui retains callback surfaces after submission. Keep our handle on a
    // failed submit so cleanup can retry rather than orphaning a visible marker.
    if (status == NO_ERROR || status == DEAD_OBJECT) {
        gSurface.clear();
        gBuffer.clear();
    }
    std::string result = "transaction_failed";
    bool presented = false;
    if (status == NO_ERROR) {
        const auto deadline = started + std::chrono::milliseconds(500);
        std::unique_lock lock(pending->mutex);
        pending->condition.wait_until(lock, deadline, [&pending] { return pending->completed; });
        const sp<Fence> fence = pending->fence;
        if (!pending->completed) {
            result = "complete_callback_timeout";
        } else if (fence == nullptr || !fence->isValid()) {
            // An off display can complete a transaction without presenting a frame.
            result = "no_present_fence";
        } else {
            lock.unlock();
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - Clock::now()).count();
            const status_t waited = fence->wait(static_cast<int>(std::max<int64_t>(0, remaining)));
            presented = waited == NO_ERROR;
            result = presented ? "presented" : "present_fence_error:" + std::to_string(waited);
        }
    } else if (status == DEAD_OBJECT) {
        gClient.clear();
    }
    if (!presented) {
        gBufferCache.clear();
        cancelPreparation(true);
    }
    {
        std::lock_guard lock(gDiagnosticsMutex);
        gLastHide = {status, result};
    }
    if (!presented) ALOGW("Hide failed: status=%d result=%s", status, result.c_str());
    return presented;
}


bool show(int width, int height, int layerStack, float cx, float cy, float rx, float ry,
          float opacity, int64_t generation) {
    const auto current = [generation] { return gGeneration.load(std::memory_order_relaxed) == generation; };
    const auto pending = std::make_shared<PresentState>();
    pending->generation = generation;
    {
        std::lock_guard lock(gDiagnosticsMutex);
        gLastPresent = pending;
    }
    const auto failed = [&pending](const std::string& reason) {
        {
            std::lock_guard lock(pending->mutex);
            pending->result = reason;
        }
        if (reason.rfind("cancelled_", 0) != 0) {
            ALOGW("Presentation failed: %s", reason.c_str());
        }
        return false;
    };
    if (!hide()) return failed("previous_surface_removal_failed");
    if (!current()) return failed("cancelled_before_prepare");
    const BufferKey key{width, height, cx, cy, rx, ry, opacity};
    if (layerStack < 0 || !validKey(key)) {
        ALOGE("Invalid illumination buffer geometry");
        return failed("invalid_geometry");
    }
    if (gClient == nullptr) {
        ProcessState::self()->startThreadPool();
        gClient = sp<SurfaceComposerClient>::make();
        if (gClient->initCheck() != NO_ERROR) {
            gClient.clear();
            gBufferCache.clear();
            cancelPreparation(true);
            ALOGE("Unable to connect to SurfaceFlinger");
            return failed("surfaceflinger_connection_failed");
        }
    }
    gBuffer = gBufferCache.find(key);
    {
        std::lock_guard lock(pending->mutex);
        pending->bufferCacheHit = gBuffer != nullptr;
    }
    if (gBuffer == nullptr) {
        auto ready = takePreparedBuffer(key);
        if (ready.buffer != nullptr) {
            gBuffer = std::move(ready.buffer);
            std::lock_guard lock(pending->mutex);
            pending->bufferPrepared = true;
        }
    }
    if (gBuffer == nullptr) {
        gBuffer = sp<GraphicBuffer>::make(width, height, PIXEL_FORMAT_RGBA_8888, 1,
                GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_TEXTURE,
                "NTFingerprintDimLayer");
        if (gBuffer->initCheck() != NO_ERROR) {
            ALOGE("Unable to allocate illumination buffer");
            gBuffer.clear();
            return failed("buffer_allocation_failed");
        }
        // The MTK composer reads gralloc NAME, not the SurfaceControl debug name, to
        // place HBM_ENABLE in the atomic commit carrying this buffer (also with GPU composition).
        std::string bufferName;
        if (GraphicBufferMapper::get().getName(gBuffer->handle, &bufferName) != NO_ERROR
                || bufferName.find("NTFingerprintDimLayer") == std::string::npos) {
            gBuffer.clear();
            return failed("missing_composer_buffer_marker");
        }
        void* address = nullptr;
        if (gBuffer->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &address) != NO_ERROR || address == nullptr) {
            gBuffer.clear();
            return failed("buffer_lock_failed");
        }
        tetris::udfps::fillIllumination(static_cast<uint8_t*>(address), width, height,
                                      gBuffer->getStride(), cx, cy, rx, ry, opacity);
        if (gBuffer->unlock() != NO_ERROR) {
            gBuffer.clear();
            return failed("buffer_unlock_failed");
        }
    }
    if (!current()) {
        hide();
        return failed("cancelled_after_prepare");
    }
    gBufferCache.insert(key, gBuffer);
    {
        std::lock_guard lock(pending->mutex);
        pending->bufferCacheEntries = gBufferCache.size();
    }
    gSurface = gClient->createSurface(String8("NTFingerprintDimLayer Tetris"), width, height,
            PIXEL_FORMAT_RGBA_8888,
            gui::ISurfaceComposerClient::eFXSurfaceBufferState
                    | gui::ISurfaceComposerClient::eSecure);
    if (gSurface == nullptr || !gSurface->isValid()) {
        hide();
        // A dead SurfaceFlinger invalidates the connection; reconnect on the next attempt.
        gClient.clear();
        gBufferCache.clear();
        cancelPreparation(true);
        return failed("surface_creation_failed");
    }
    const auto target = gSurface;
    // Readiness requires this buffer to latch and its completed transaction's
    // present fence to signal. A transaction commit alone does not prove either.
    auto callback = [pending, target](void*, nsecs_t, const sp<Fence>& fence,
                                     const std::vector<SurfaceControlStats>& stats) {
        std::lock_guard lock(pending->mutex);
        for (const auto& stat : stats) {
            if (stat.surfaceControl == target && stat.latchTime >= 0) {
                pending->latched = true;
                break;
            }
        }
        pending->fence = fence;
        pending->completed = true;
        pending->condition.notify_all();
    };
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    if (!current()) {
        hide();
        return failed("cancelled_before_submit");
    }
    {
        std::lock_guard lock(pending->mutex);
        pending->result = "waiting_complete_callback";
    }
    status_t status = SurfaceComposerClient::Transaction()
            .setLayerStack(gSurface, ui::LayerStack::fromValue(static_cast<uint32_t>(layerStack)))
            .setLayer(gSurface, std::numeric_limits<int32_t>::max() - 16)
            .setTrustedOverlay(gSurface, true)
            .setDataspace(gSurface, ui::Dataspace::V0_SRGB)
            .setBuffer(gSurface, gBuffer)
            .show(gSurface)
            .addTransactionCompletedCallback(std::move(callback), nullptr)
            .apply();
    if (status != NO_ERROR) {
        ALOGE("Illumination transaction failed: %d", status);
        hide();
        if (status == DEAD_OBJECT) {
            gClient.clear();
            gBufferCache.clear();
            cancelPreparation(true);
        }
        return failed("transaction_failed:" + std::to_string(status));
    }
    std::unique_lock lock(pending->mutex);
    while (!pending->completed && current() && Clock::now() < deadline) {
        pending->condition.wait_until(lock, std::min(deadline,
                Clock::now() + std::chrono::milliseconds(20)));
    }
    if (!current() || !pending->completed || !pending->latched
            || pending->fence == nullptr || !pending->fence->isValid()) {
        const std::string reason = !current() ? "cancelled_waiting_callback"
                : !pending->completed ? "complete_callback_timeout"
                : !pending->latched ? "surface_not_latched"
                : "invalid_present_fence";
        lock.unlock();
        hide();
        return failed(reason);
    }
    const sp<Fence> presentFence = pending->fence;
    pending->result = "waiting_present_fence";
    lock.unlock();
    status_t finalWaitStatus = TIMED_OUT;
    while (current()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count();
        if (remaining < 0) break;
        const status_t waitStatus = presentFence->wait(static_cast<int>(std::min<int64_t>(remaining, 20)));
        finalWaitStatus = waitStatus;
        if (waitStatus == NO_ERROR && current()) {
            {
                std::lock_guard completeLock(pending->mutex);
                pending->result = "presented";
            }
            return true;
        }
        if (waitStatus != -ETIME && waitStatus != TIMED_OUT) break;
    }
    const std::string reason = !current() ? "cancelled_waiting_fence"
            : finalWaitStatus == -ETIME || finalWaitStatus == TIMED_OUT
                    ? "present_fence_timeout" : "present_fence_error:" + std::to_string(finalWaitStatus);
    hide();
    return failed(reason);
}
} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeShow(
        JNIEnv*, jclass, jint width, jint height, jint layerStack, jfloat x, jfloat y,
        jfloat radiusX, jfloat radiusY, jfloat alpha, jlong generation) {
    return show(width, height, layerStack, x, y, radiusX, radiusY, alpha, generation);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeHide(JNIEnv*, jclass) {
    return hide();
}

extern "C" JNIEXPORT void JNICALL
Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeSetGeneration(
        JNIEnv*, jclass, jlong generation) {
    gGeneration.store(generation, std::memory_order_relaxed);
    cancelPreparation();
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeCancelPreparation(JNIEnv*, jclass) {
    return static_cast<jlong>(cancelPreparation());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeHasBuffer(
        JNIEnv*, jclass, jint width, jint height, jfloat x, jfloat y,
        jfloat radiusX, jfloat radiusY, jfloat alpha) {
    const BufferKey key{width, height, x, y, radiusX, radiusY, alpha};
    if (!validKey(key)) return false;
    if (gBufferCache.find(key) != nullptr) return true;
    std::lock_guard lock(gPreparationMutex);
    return gPreparedBuffer.buffer != nullptr && gPreparedBuffer.key == key;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_lineageos_tetris_udfps_IlluminationApplication_nativePrepareBuffer(
        JNIEnv*, jclass, jint width, jint height, jfloat x, jfloat y,
        jfloat radiusX, jfloat radiusY, jfloat alpha, jlong token) {
    if (token <= 0) return false;
    return prepareBuffer({width, height, x, y, radiusX, radiusY, alpha},
                         static_cast<uint64_t>(token));
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeGetDiagnostics(
        JNIEnv* env, jclass) {
    return env->NewStringUTF(diagnostics().c_str());
}
