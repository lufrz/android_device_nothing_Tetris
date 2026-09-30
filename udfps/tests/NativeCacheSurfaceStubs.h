// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define JNIEXPORT
#define JNICALL
using jboolean = bool;
using jint = int;
using jlong = int64_t;
using jfloat = float;
using jclass = void*;
using jstring = const char*;
struct JNIEnv { jstring NewStringUTF(const char* value) { return value; } };
#define ALOGI(...) ((void)0)
#define ALOGW(...) ((void)0)
#define ALOGE(...) ((void)0)
constexpr uint64_t GRALLOC_USAGE_SW_WRITE_OFTEN = 1;
constexpr uint64_t GRALLOC_USAGE_HW_COMPOSER = 2;
constexpr uint64_t GRALLOC_USAGE_HW_TEXTURE = 4;

namespace native_test {
struct State {
    int allocations = 0, locks = 0, unlocks = 0, markerReads = 0;
    int clients = 0, surfaces = 0, shows = 0, hides = 0;
    int showCallbacks = 0, hideCallbacks = 0, commits = 0;
    int showWaits = 0, hideWaits = 0, bufferSubmits = 0;
    int allocationError = 0, markerError = 0, lockError = 0, unlockError = 0;
    int clientError = 0, showError = 0, hideError = 0;
    int showFenceError = 0, hideFenceError = 0;
    bool badMarker = false, nullAddress = false, noSurface = false, invalidSurface = false;
    bool showLatch = true, wrongSurface = false, showCallback = true;
    bool showFenceValid = true, hideFenceValid = true, showNullFence = false;
    std::function<void()> onLock, onUnlock, onCreate, onApply, onShowFence;
    uint32_t lastLayerStack = 0;
    int64_t lastLayer = 0;
    bool trusted = false, secure = false, detachedBeforeHide = false;
};
inline State state;
}

namespace android {
using status_t = int;
using nsecs_t = int64_t;
constexpr status_t NO_ERROR = 0, DEAD_OBJECT = -32, TIMED_OUT = -110;
constexpr int PIXEL_FORMAT_RGBA_8888 = 1;
template <typename T> class sp {
public:
    sp() = default;
    sp(std::nullptr_t) {}
    explicit sp(std::shared_ptr<T> value) : ptr(std::move(value)) {}
    template <typename... Args> static sp make(Args&&... args) {
        return sp(std::make_shared<T>(std::forward<Args>(args)...));
    }
    T* operator->() const { return ptr.get(); }
    T* get() const { return ptr.get(); }
    void clear() { ptr.reset(); }
    bool operator==(std::nullptr_t) const { return !ptr; }
    bool operator!=(std::nullptr_t) const { return bool(ptr); }
    bool operator==(const sp& other) const { return ptr == other.ptr; }
    bool operator!=(const sp& other) const { return ptr != other.ptr; }
private:
    std::shared_ptr<T> ptr;
};
struct String8 { explicit String8(const char*) {} };
namespace ui {
struct LayerStack { uint32_t value; static LayerStack fromValue(uint32_t v) { return {v}; } };
enum class Dataspace { V0_SRGB };
}
namespace gui { struct ISurfaceComposerClient {
    static constexpr int eFXSurfaceBufferState = 1, eSecure = 2;
}; }
struct ProcessState {
    static ProcessState* self() { static ProcessState instance; return &instance; }
    void startThreadPool() {}
};
class GraphicBuffer {
public:
    GraphicBuffer(int width, int height, int format, int layers, uint64_t usage, const char* name)
          : width(width), height(height), stride(width + 3), name(name),
            bytes(static_cast<size_t>(stride) * height * 4, 0xa5), handle(this) {
        ++native_test::state.allocations;
        assert(format == PIXEL_FORMAT_RGBA_8888 && layers == 1);
        assert(usage == (GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_HW_COMPOSER |
                         GRALLOC_USAGE_HW_TEXTURE));
    }
    int initCheck() const { return native_test::state.allocationError; }
    int lock(uint64_t usage, void** address) {
        ++native_test::state.locks;
        assert(usage == GRALLOC_USAGE_SW_WRITE_OFTEN);
        *address = native_test::state.nullAddress ? nullptr : bytes.data();
        if (native_test::state.onLock) native_test::state.onLock();
        return native_test::state.lockError;
    }
    int unlock() {
        ++native_test::state.unlocks;
        if (native_test::state.onUnlock) native_test::state.onUnlock();
        return native_test::state.unlockError;
    }
    uint32_t getStride() const { return stride; }
    int width, height;
    uint32_t stride;
    std::string name;
    std::vector<uint8_t> bytes;
    void* handle;
};
struct GraphicBufferMapper {
    static GraphicBufferMapper& get() { static GraphicBufferMapper instance; return instance; }
    int getName(void* handle, std::string* name) {
        ++native_test::state.markerReads;
        *name = native_test::state.badMarker ? "unmarked" : static_cast<GraphicBuffer*>(handle)->name;
        return native_test::state.markerError;
    }
};
class SurfaceControl {
public:
    bool isValid() const { return valid; }
    bool valid = true;
};
class Fence {
public:
    explicit Fence(bool show) : forShow(show) {}
    bool isValid() const {
        return forShow ? native_test::state.showFenceValid : native_test::state.hideFenceValid;
    }
    int wait(int timeout) {
        assert(timeout >= 0 && timeout <= 500);
        if (forShow) {
            ++native_test::state.showWaits;
            if (native_test::state.onShowFence) native_test::state.onShowFence();
            return native_test::state.showFenceError;
        }
        ++native_test::state.hideWaits;
        return native_test::state.hideFenceError;
    }
    bool forShow;
};
struct SurfaceControlStats { sp<SurfaceControl> surfaceControl; nsecs_t latchTime; };
// Retain compositor references across cache eviction to expose in-place mutations.
inline std::vector<sp<GraphicBuffer>> compositorBuffers;
inline std::vector<sp<SurfaceControl>> submittedSurfaces;
class SurfaceComposerClient {
public:
    SurfaceComposerClient() { ++native_test::state.clients; }
    int initCheck() const { return native_test::state.clientError; }
    sp<SurfaceControl> createSurface(String8, int, int, int, int flags) {
        ++native_test::state.surfaces;
        native_test::state.secure = (flags & gui::ISurfaceComposerClient::eSecure) != 0;
        if (native_test::state.onCreate) native_test::state.onCreate();
        if (native_test::state.noSurface) return nullptr;
        auto surface = sp<SurfaceControl>::make();
        surface->valid = !native_test::state.invalidSurface;
        return surface;
    }
    class Transaction {
    public:
        using Callback = std::function<void(void*, nsecs_t, const sp<Fence>&,
                                            const std::vector<SurfaceControlStats>&)>;
        Transaction& setLayerStack(const sp<SurfaceControl>&, ui::LayerStack stack) {
            native_test::state.lastLayerStack = stack.value; return *this;
        }
        Transaction& setLayer(const sp<SurfaceControl>&, int64_t layer) {
            native_test::state.lastLayer = layer; return *this;
        }
        Transaction& setTrustedOverlay(const sp<SurfaceControl>&, bool trusted) {
            native_test::state.trusted = trusted; return *this;
        }
        Transaction& setDataspace(const sp<SurfaceControl>&, ui::Dataspace) { return *this; }
        Transaction& setBuffer(const sp<SurfaceControl>& surface, const sp<GraphicBuffer>& value) {
            target = surface; buffer = value; setBufferCalled = true; return *this;
        }
        Transaction& hide(const sp<SurfaceControl>& surface) {
            target = surface; isHide = true;
            native_test::state.detachedBeforeHide = setBufferCalled && buffer == nullptr;
            return *this;
        }
        Transaction& show(const sp<SurfaceControl>& surface) { target = surface; return *this; }
        Transaction& addTransactionCompletedCallback(Callback cb, void*) {
            complete = std::move(cb); return *this;
        }
        Transaction& addTransactionCommittedCallback(Callback cb, void*) {
            committed = std::move(cb); return *this;
        }
        int apply() {
            auto& state = native_test::state;
            assert(target != nullptr && setBufferCalled);
            if (isHide) {
                ++state.hides;
                assert(buffer == nullptr && state.detachedBeforeHide);
                if (state.hideError) return state.hideError;
                assert(bool(complete));
                ++state.hideCallbacks;
                complete(nullptr, 0, sp<Fence>::make(false), {});
                return NO_ERROR;
            }
            ++state.shows;
            assert(buffer != nullptr && state.secure && state.trusted);
            ++state.bufferSubmits;
            submittedSurfaces.push_back(target);
            compositorBuffers.push_back(buffer);
            if (state.onApply) state.onApply();
            if (state.showError) return state.showError;
            if (committed) { ++state.commits; committed(nullptr, 0, nullptr, {}); }
            if (state.showCallback) {
                assert(bool(complete));
                auto surface = state.wrongSurface ? sp<SurfaceControl>::make() : target;
                auto fence = state.showNullFence ? sp<Fence>{} : sp<Fence>::make(true);
                ++state.showCallbacks;
                complete(nullptr, 0, fence, {{surface, state.showLatch ? 1 : -1}});
            }
            return NO_ERROR;
        }
    private:
        sp<SurfaceControl> target;
        sp<GraphicBuffer> buffer;
        bool isHide = false, setBufferCalled = false;
        Callback complete, committed;
    };
};
}
