// SPDX-License-Identifier: Apache-2.0
#include "DownmixHook.h"

#include <atomic>
#include <array>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <mutex>

namespace {
constexpr char kDownmixSymbol[] =
        "_ZN7android28AudioALSAPlaybackHandlerBase30doStereoToMonoConversionIfNeedEPvm";
constexpr char kMixerSymbol[] =
        "_ZN7android28AudioALSAPlaybackHandlerBase16wrapSwMixerWriteEPvjS1_";
// Compatibility fingerprints of the audited Tetris functions, not security hashes.
constexpr uint64_t kDownmixFingerprint = 0xe2965e82d5d4b081ULL;
constexpr uint64_t kMixerFingerprint = 0x0419a5ee9ae72d16ULL;
std::atomic<TetrisOriginalDownmix> original{nullptr};
std::atomic<TetrisOriginalMixerWrite> originalMixer{nullptr};
std::atomic<bool> compatible{false};
std::atomic<uint64_t> synchronousBypasses{0}, workerBypasses{0}, originalCalls{0};
constexpr std::array<const char*, 4> kBackendVtables{
    "_ZTVN7android30AudioALSAPlaybackHandlerNormalE",
    "_ZTVN7android28AudioALSAPlaybackHandlerFastE",
    "_ZTVN7android27AudioALSAPlaybackHandlerDspE",
    "_ZTVN7android44AudioALSAPlaybackHandlerSpeakerProtectionDspE",
};
std::array<std::atomic<uintptr_t>, 4> backendVtables{};
std::array<std::atomic<uint64_t>, 5> backendCalls{};
thread_local TetrisHookScope* scope = nullptr;
std::mutex bindingMutex;
void* boundHandler = nullptr;
uint64_t boundGeneration = 0;

struct Mapping {
    uintptr_t address;
    size_t bytes;
    bool readable = false;
};
int findMapping(dl_phdr_info* info, size_t, void* opaque) {
    auto* mapping = static_cast<Mapping*>(opaque);
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || !(phdr.p_flags & PF_R) || !(phdr.p_flags & PF_X)) continue;
        const uintptr_t begin = info->dlpi_addr + phdr.p_vaddr;
        if (mapping->address >= begin && mapping->address - begin <= phdr.p_memsz &&
                mapping->bytes <= phdr.p_memsz - (mapping->address - begin)) {
            mapping->readable = true;
            return 1;
        }
    }
    return 0;
}
bool knownFunction(void* function, size_t bytes, uint64_t fingerprint) {
    Mapping mapping{reinterpret_cast<uintptr_t>(function), bytes};
    dl_iterate_phdr(findMapping, &mapping);
#if defined(__aarch64__)
    if (mapping.readable) {
        uint64_t hash = 14695981039346656037ULL;
        const auto* code = static_cast<const uint8_t*>(function);
        for (size_t i = 0; i < bytes; ++i) hash = (hash ^ code[i]) * 1099511628211ULL;
        return hash == fingerprint;
    }
#else
    (void)fingerprint;
#endif
    return false;
}
bool authorized(const TetrisHookScope& current, void* handler) {
    if (!handler || current.handler != handler || !current.generation ||
            !compatible.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(bindingMutex);
    return handler == boundHandler && current.generation == boundGeneration;
}
void countBackend(void* handler) {
    // The four audited constructors store their primary address point at self:
    // vtable symbol + two pointer-sized ABI header entries. No other fields are read.
    uintptr_t vtable;
    std::memcpy(&vtable, handler, sizeof(vtable));
    size_t backend = backendVtables.size();
    for (size_t i = 0; i < backendVtables.size(); ++i) {
        const auto expected = backendVtables[i].load(std::memory_order_relaxed);
        if (expected && expected == vtable) { backend = i; break; }
    }
    backendCalls[backend].fetch_add(1, std::memory_order_relaxed);
}
struct RestoreScope {
    TetrisHookScope* previous;
    ~RestoreScope() { scope = previous; }
};
}

extern "C" int tetris_audio_vendor_downmix(void* self, void* buffer, size_t bytes)
        asm("_ZN7android28AudioALSAPlaybackHandlerBase30doStereoToMonoConversionIfNeedEPvm");
extern "C" int tetris_audio_vendor_downmix(void* self, void* buffer, size_t bytes) {
    if (scope && scope->observer) {
        auto& observation = *scope->observer;
        if (!self || (observation.observedHandler && observation.observedHandler != self)) {
            observation.ambiguous = true;
        } else {
            observation.observedHandler = self;
        }
    }
    if (scope && authorized(*scope, self)) {
        (scope->worker ? workerBypasses : synchronousBypasses).fetch_add(1, std::memory_order_relaxed);
        countBackend(self);
        return 0;
    }
    originalCalls.fetch_add(1, std::memory_order_relaxed);
    auto function = original.load(std::memory_order_acquire);
    return function ? function(self, buffer, bytes) : -ENODEV;
}

extern "C" int tetris_audio_vendor_mixer_write(void* buffer, unsigned int bytes, void* handler)
        asm("_ZN7android28AudioALSAPlaybackHandlerBase16wrapSwMixerWriteEPvjS1_");
extern "C" int tetris_audio_vendor_mixer_write(void* buffer, unsigned int bytes, void* handler) {
    auto function = originalMixer.load(std::memory_order_acquire);
    if (!function) return -ENODEV;
    TetrisHookScope nested{};
    nested.handler = handler;
    nested.previous = scope;
    if (scope) {
        nested.observer = scope->observer;
        nested.generation = scope->handler == handler ? scope->generation : 0;
        nested.worker = scope->worker;
    } else {
        // The vendor mixer can move a previously synchronous target onto its
        // worker. Only that exact target may inherit this live stereo lease.
        std::lock_guard lock(bindingMutex);
        if (handler == boundHandler) nested.generation = boundGeneration;
        nested.worker = true;
    }
    RestoreScope restore{scope};
    scope = &nested;
    // No hook lock is held across vendor code. Downmix rechecks the generation
    // when invoked, so a callback paused across lease revocation stays stock.
    return function(buffer, bytes, handler);
}

extern "C" bool tetris_audio_hook_initialize(void* vendorHandle) {
    auto downmix = reinterpret_cast<TetrisOriginalDownmix>(dlsym(vendorHandle, kDownmixSymbol));
    auto mixer = reinterpret_cast<TetrisOriginalMixerWrite>(dlsym(vendorHandle, kMixerSymbol));
    if (!downmix || downmix == tetris_audio_vendor_downmix) return false;
    if (mixer == tetris_audio_vendor_mixer_write) mixer = nullptr;
    original.store(downmix, std::memory_order_release);
    originalMixer.store(mixer, std::memory_order_release);
    for (size_t i = 0; i < backendVtables.size(); ++i) {
        const auto table = reinterpret_cast<uintptr_t>(dlsym(vendorHandle, kBackendVtables[i]));
        backendVtables[i].store(table ? table + 2 * sizeof(void*) : 0, std::memory_order_relaxed);
    }
    compatible.store(knownFunction(reinterpret_cast<void*>(downmix), 0x200, kDownmixFingerprint) &&
            mixer && knownFunction(reinterpret_cast<void*>(mixer), 0x220, kMixerFingerprint),
            std::memory_order_release);
    tetris_audio_hook_clear();
    return true;
}
extern "C" bool tetris_audio_hook_compatible() {
    return compatible.load(std::memory_order_acquire);
}
extern "C" uint64_t tetris_audio_hook_bind(void* handler) {
    std::lock_guard lock(bindingMutex);
    ++boundGeneration;
    boundHandler = handler && compatible.load(std::memory_order_acquire) ? handler : nullptr;
    return boundHandler ? boundGeneration : 0;
}
extern "C" void tetris_audio_hook_clear() {
    std::lock_guard lock(bindingMutex);
    boundHandler = nullptr;
    ++boundGeneration;
}
extern "C" void tetris_audio_hook_begin(TetrisHookScope* current, void* handler, uint64_t generation) {
    *current = {};
    current->previous = scope;
    current->observer = current;
    current->handler = handler;
    current->generation = generation;
    scope = current;
}
extern "C" void tetris_audio_hook_end(TetrisHookScope* current) {
    scope = current->previous;
}
extern "C" TetrisHookStats tetris_audio_hook_stats() {
    return {synchronousBypasses.load(std::memory_order_relaxed),
            workerBypasses.load(std::memory_order_relaxed), originalCalls.load(std::memory_order_relaxed),
            backendCalls[0].load(std::memory_order_relaxed), backendCalls[1].load(std::memory_order_relaxed),
            backendCalls[2].load(std::memory_order_relaxed), backendCalls[3].load(std::memory_order_relaxed),
            backendCalls[4].load(std::memory_order_relaxed)};
}
bool tetris_audio_hook_authorized_scope() {
    return scope && authorized(*scope, scope->handler);
}
