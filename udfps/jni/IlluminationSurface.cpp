/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "TetrisUdfpsSurface"

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
std::atomic<int64_t> gGeneration{0};

struct PresentState {
    std::mutex mutex;
    std::condition_variable condition;
    const Clock::time_point started = Clock::now();
    int64_t generation = 0;
    int64_t connectMs = -1;
    int64_t bufferPrepareMs = -1;
    int64_t rasterMs = -1;
    int64_t bufferUnlockMs = -1;
    int64_t surfaceCreateMs = -1;
    int64_t submittedMs = -1;
    int64_t committedMs = -1;
    int64_t completedMs = -1;
    int64_t fenceMs = -1;
    std::string result = "preparing";
    bool completed = false;
    bool latched = false;
    sp<Fence> fence;
};

std::mutex gDiagnosticsMutex;
std::shared_ptr<PresentState> gLastPresent;
std::string gLastHide = "none";

int64_t elapsedMs(Clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

std::string diagnostics() {
    std::shared_ptr<PresentState> pending;
    std::string lastHide;
    {
        std::lock_guard lock(gDiagnosticsMutex);
        pending = gLastPresent;
        lastHide = gLastHide;
    }
    std::ostringstream out;
    if (pending != nullptr) {
        std::lock_guard lock(pending->mutex);
        out << "generation=" << pending->generation << " result=" << pending->result
            << " started_uptime_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                    pending->started.time_since_epoch()).count()
            << " connect_ms=" << pending->connectMs
            << " buffer_prepare_ms=" << pending->bufferPrepareMs
            << " raster_ms=" << pending->rasterMs
            << " buffer_unlock_ms=" << pending->bufferUnlockMs
            << " surface_create_ms=" << pending->surfaceCreateMs
            << " submit_ms=" << pending->submittedMs
            << " commit_callback_ms=" << pending->committedMs
            << " complete_callback_ms=" << pending->completedMs
            << " fence_observed_ms=" << pending->fenceMs;
    } else {
        out << "presentation=none";
    }
    out << " hide={" << lastHide << "}";
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
    std::ostringstream timing;
    timing << "started_uptime_ms="
           << std::chrono::duration_cast<std::chrono::milliseconds>(started.time_since_epoch()).count()
           << " total_ms=" << elapsedMs(started) << " status=" << status << " result=" << result;
    {
        std::lock_guard lock(gDiagnosticsMutex);
        gLastHide = timing.str();
    }
    ALOGI("Hide: %s", timing.str().c_str());
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
        ALOGW("Presentation failed: %s", diagnostics().c_str());
        return false;
    };
    if (!hide()) return failed("previous_surface_removal_failed");
    if (!current()) return failed("cancelled_before_prepare");
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || layerStack < 0
            || !std::isfinite(opacity) || opacity < 0 || opacity > 1
            || !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(rx)
            || !std::isfinite(ry) || rx <= 0 || ry <= 0
            || cx - rx < 0 || cy - ry < 0 || cx + rx > width || cy + ry > height) {
        ALOGE("Invalid illumination buffer geometry");
        return failed("invalid_geometry");
    }
    const auto recordPhase = [&pending](int64_t PresentState::*field, Clock::time_point started) {
        std::lock_guard lock(pending->mutex);
        pending.get()->*field = elapsedMs(started);
    };
    auto phaseStarted = Clock::now();
    if (gClient == nullptr) {
        ProcessState::self()->startThreadPool();
        gClient = sp<SurfaceComposerClient>::make();
        if (gClient->initCheck() != NO_ERROR) {
            gClient.clear();
            ALOGE("Unable to connect to SurfaceFlinger");
            return failed("surfaceflinger_connection_failed");
        }
    }
    recordPhase(&PresentState::connectMs, phaseStarted);
    phaseStarted = Clock::now();
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
    recordPhase(&PresentState::bufferPrepareMs, phaseStarted);
    phaseStarted = Clock::now();
    tetris::udfps::fillIllumination(static_cast<uint8_t*>(address), width, height,
                                  gBuffer->getStride(), cx, cy, rx, ry, opacity);
    recordPhase(&PresentState::rasterMs, phaseStarted);
    phaseStarted = Clock::now();
    if (gBuffer->unlock() != NO_ERROR) {
        gBuffer.clear();
        return failed("buffer_unlock_failed");
    }
    recordPhase(&PresentState::bufferUnlockMs, phaseStarted);
    if (!current()) {
        hide();
        return failed("cancelled_after_prepare");
    }
    phaseStarted = Clock::now();
    gSurface = gClient->createSurface(String8("NTFingerprintDimLayer Tetris"), width, height,
            PIXEL_FORMAT_RGBA_8888,
            gui::ISurfaceComposerClient::eFXSurfaceBufferState
                    | gui::ISurfaceComposerClient::eSecure);
    if (gSurface == nullptr || !gSurface->isValid()) {
        hide();
        // A dead SurfaceFlinger invalidates the connection; reconnect on the next attempt.
        gClient.clear();
        return failed("surface_creation_failed");
    }
    recordPhase(&PresentState::surfaceCreateMs, phaseStarted);
    const auto target = gSurface;
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
        pending->completedMs = elapsedMs(pending->started);
        pending->completed = true;
        pending->condition.notify_all();
    };
    // Commit is measured only; sensor readiness waits for the completed callback and present fence.
    auto committed = [pending](void*, nsecs_t, const sp<Fence>&,
                               const std::vector<SurfaceControlStats>&) {
        std::lock_guard lock(pending->mutex);
        pending->committedMs = elapsedMs(pending->started);
    };
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    if (!current()) {
        hide();
        return failed("cancelled_before_submit");
    }
    {
        std::lock_guard lock(pending->mutex);
        pending->submittedMs = elapsedMs(pending->started);
        pending->result = "waiting_complete_callback";
    }
    status_t status = SurfaceComposerClient::Transaction()
            .setLayerStack(gSurface, ui::LayerStack::fromValue(static_cast<uint32_t>(layerStack)))
            .setLayer(gSurface, std::numeric_limits<int32_t>::max() - 16)
            .setTrustedOverlay(gSurface, true)
            .setDataspace(gSurface, ui::Dataspace::V0_SRGB)
            .setBuffer(gSurface, gBuffer)
            .show(gSurface)
            .addTransactionCommittedCallback(std::move(committed), nullptr)
            .addTransactionCompletedCallback(std::move(callback), nullptr)
            .apply();
    if (status != NO_ERROR) {
        ALOGE("Illumination transaction failed: %d", status);
        hide();
        if (status == DEAD_OBJECT) gClient.clear();
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
                : !pending->completed ? (pending->committedMs < 0
                        ? "callback_timeout_without_commit" : "complete_callback_timeout")
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
                pending->fenceMs = elapsedMs(pending->started);
                pending->result = "presented";
            }
            ALOGI("Presentation: %s", diagnostics().c_str());
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
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_lineageos_tetris_udfps_IlluminationApplication_nativeGetDiagnostics(
        JNIEnv* env, jclass) {
    return env->NewStringUTF(diagnostics().c_str());
}
