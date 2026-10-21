// SPDX-License-Identifier: Apache-2.0
#include "BessoundProbe.h"
#include "DownmixHook.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <mutex>

namespace bessound_probe {
namespace {
// Binary ABI of the Tetris vendor ARSI callback. The middle two arguments are
// opaque and forwarded unchanged. This probe never calls a DSP mode setter.
using Process = int (*)(void*, void*, void*, void*, void*);
constexpr char kProcessSymbol[] = "audioloud_arsi_process_dl_buf";
constexpr char kWrapperPath[] = "/vendor/lib64/libaudioloudc.so";
constexpr char kEnginePath[] = "/vendor/lib64/libbessound_hd_mtk_vendor.so";

struct LibraryFingerprint {
    uintptr_t functionOffset;
    uintptr_t textOffset;
    size_t textBytes;
    uint64_t textHash;
};
// FNV-1a compatibility hashes of the entire unrelocated .text sections, not
// security hashes. Unknown vendor releases are forwarded without inspection.
// Wrapper build ID: 4e0980e9ef542b3bd42cfb72a19d1f19.
constexpr LibraryFingerprint kWrapper{0x4f60, 0x3000, 0x370c, 0x7b33bf3bc466f742ULL};
// Engine build ID: 2757a657f7985f837c0677834c5be5b0.
constexpr LibraryFingerprint kEngine{0xa420, 0x6000, 0xeeec, 0x0b36d3230e3dd209ULL};
constexpr size_t kMaxFrames = 64;

std::atomic<Process> originalProcess{nullptr};
std::once_flag resolveOnce;
std::atomic<bool> audited{false};
std::atomic<bool> wrapperMatched{false}, engineMatched{false};
uintptr_t engineBase = 0;
std::atomic<uint64_t> totalCalls{0}, unscopedCalls{0};
std::atomic<uint64_t> scopedCalls{0}, monoCalls{0}, stereoCalls{0}, unknownCalls{0};
std::atomic<uint64_t> inputFrames{0}, inputDifferent{0}, outputFrames{0}, outputDifferent{0};

struct TextMapping {
    uintptr_t base;
    const LibraryFingerprint* fingerprint;
    bool matched = false;
};
int checkText(dl_phdr_info* info, size_t, void* opaque) {
    auto& mapping = *static_cast<TextMapping*>(opaque);
    if (info->dlpi_addr != mapping.base) return 0;
    const auto& expected = *mapping.fingerprint;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_R) ||
                !(segment.p_flags & PF_X) || expected.textOffset < segment.p_vaddr) continue;
        const auto offset = expected.textOffset - segment.p_vaddr;
        if (offset > segment.p_memsz || expected.textBytes > segment.p_memsz - offset) continue;
        const auto* text = reinterpret_cast<const uint8_t*>(mapping.base + expected.textOffset);
        uint64_t hash = 14695981039346656037ULL;
        for (size_t byte = 0; byte < expected.textBytes; ++byte) {
            hash = (hash ^ text[byte]) * 1099511628211ULL;
        }
        mapping.matched = hash == expected.textHash;
        break;
    }
    return 1;
}
bool matchesLibrary(void* function, const LibraryFingerprint& expected, uintptr_t& base) {
    Dl_info info{};
    if (!function || !dladdr(function, &info) || !info.dli_fbase) return false;
    base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    if (reinterpret_cast<uintptr_t>(function) - base != expected.functionOffset) return false;
    TextMapping mapping{base, &expected};
    dl_iterate_phdr(checkText, &mapping);
    return mapping.matched;
}

// memcpy avoids alignment assumptions about opaque vendor structures.
template <typename T> T field(const void* object, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, sizeof(value));
    return value;
}

bool inspectHandler(void* handler, uint32_t& mono, uint32_t& stereo) {
    if (!handler) return false;
    // These callbacks and self-contained pointers identify the audited wrapper
    // allocation before following its engine state. Pointers remain callback-local.
    if (field<uintptr_t>(handler, 0x08) != engineBase + 0x13970 ||
            field<uintptr_t>(handler, 0x10) != engineBase + 0x13840 ||
            field<uintptr_t>(handler, 0x28) != engineBase + 0x13a50 ||
            field<uintptr_t>(handler, 0x50) != reinterpret_cast<uintptr_t>(handler) + 0x640 ||
            field<uintptr_t>(handler, 0x30) != reinterpret_cast<uintptr_t>(handler) + 0xc84 ||
            field<uint32_t>(handler, 0x38) != 2 || field<uint32_t>(handler, 0x40) != 1) return false;
    const auto* state = static_cast<const uint8_t*>(handler) + 0xc84;
    stereo = field<uint32_t>(state, 0);
    mono = field<uint32_t>(state, 8);
    return stereo <= 1 && mono <= 1;
}

struct ChannelCounts { uint64_t frames = 0, different = 0; };
ChannelCounts countChannels(const void* descriptor) {
    if (!descriptor) return {};
    const auto bytes = field<uint32_t>(descriptor, 4);
    const auto* pcm = field<const uint8_t*>(descriptor, 8);
    if (!pcm || bytes % (2 * sizeof(int32_t))) return {};
    ChannelCounts result{std::min<size_t>(bytes / (2 * sizeof(int32_t)), kMaxFrames), 0};
    for (size_t frame = 0; frame < result.frames; ++frame) {
        result.different += field<int32_t>(pcm, frame * 8) != field<int32_t>(pcm, frame * 8 + 4);
    }
    return result;
}
} // namespace
} // namespace bessound_probe

extern "C" int audioloud_arsi_process_dl_buf(void*, void*, void*, void*, void*);

namespace bessound_probe {
namespace {
void resolveOriginal() {
    // A callback can only be reached after the ARSI library is loaded. NOLOAD
    // avoids loading or initializing an effect merely to expose diagnostics.
    // Keep the acquired references for the lifetime of the forwarding hook.
    void* wrapper = dlopen(kWrapperPath, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    auto process = wrapper ? reinterpret_cast<Process>(dlsym(wrapper, kProcessSymbol)) : nullptr;
    if (process == audioloud_arsi_process_dl_buf) process = nullptr;
    if (!process) {
        process = reinterpret_cast<Process>(dlsym(RTLD_NEXT, kProcessSymbol));
        if (process == audioloud_arsi_process_dl_buf) process = nullptr;
    }
    originalProcess.store(process, std::memory_order_release);
    uintptr_t wrapperBase = 0;
    const bool wrapperMatches =
            matchesLibrary(reinterpret_cast<void*>(process), kWrapper, wrapperBase);
    wrapperMatched.store(wrapperMatches, std::memory_order_release);
    void* engine = dlopen(kEnginePath, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    void* internal = engine ? dlsym(engine, "BLOUD_HD_Internal_Process") : nullptr;
    uintptr_t base = 0;
    const bool engineMatches = matchesLibrary(internal, kEngine, base);
    engineMatched.store(engineMatches, std::memory_order_release);
    if (!wrapperMatches || !engineMatches) return;
    engineBase = base;
    audited.store(true, std::memory_order_release);
}
} // namespace
} // namespace bessound_probe

extern "C" int audioloud_arsi_process_dl_buf(
        void* input, void* output, void* opaque2, void* opaque3, void* handler) {
    using namespace bessound_probe;
    // Count even callbacks that cannot be inspected: zero total calls differs
    // from an unrecognized library or playback outside the authorized scope.
    totalCalls.fetch_add(1, std::memory_order_relaxed);
    std::call_once(resolveOnce, resolveOriginal);
    auto process = originalProcess.load(std::memory_order_acquire);
    const bool scoped = tetris_audio_hook_authorized_scope();
    if (!scoped) {
        unscopedCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (!process) return -ENODEV;
    if (!scoped) return process(input, output, opaque2, opaque3, handler);
    scopedCalls.fetch_add(1, std::memory_order_relaxed);
    uint32_t mono = 0, stereo = 0;
    if (!audited.load(std::memory_order_acquire) || !inspectHandler(handler, mono, stereo)) {
        unknownCalls.fetch_add(1, std::memory_order_relaxed);
        return process(input, output, opaque2, opaque3, handler);
    }
    if (mono == 1 && stereo == 0) monoCalls.fetch_add(1, std::memory_order_relaxed);
    else if (mono == 0 && stereo == 1) stereoCalls.fetch_add(1, std::memory_order_relaxed);
    else unknownCalls.fetch_add(1, std::memory_order_relaxed);
    const auto before = countChannels(input);
    const int result = process(input, output, opaque2, opaque3, handler);
    // A concurrent lease revocation also ends sample inspection immediately.
    if (result == 0 && tetris_audio_hook_authorized_scope()) {
        const auto after = countChannels(output);
        inputFrames.fetch_add(before.frames, std::memory_order_relaxed);
        inputDifferent.fetch_add(before.different, std::memory_order_relaxed);
        outputFrames.fetch_add(after.frames, std::memory_order_relaxed);
        outputDifferent.fetch_add(after.different, std::memory_order_relaxed);
    }
    return result;
}

extern "C" TetrisBessoundProbeStats tetris_bessound_probe_stats() {
    using namespace bessound_probe;
    return {audited.load(std::memory_order_acquire), scopedCalls.load(std::memory_order_relaxed),
            monoCalls.load(std::memory_order_relaxed), stereoCalls.load(std::memory_order_relaxed),
            unknownCalls.load(std::memory_order_relaxed), inputFrames.load(std::memory_order_relaxed),
            inputDifferent.load(std::memory_order_relaxed), outputFrames.load(std::memory_order_relaxed),
            outputDifferent.load(std::memory_order_relaxed), totalCalls.load(std::memory_order_relaxed),
            unscopedCalls.load(std::memory_order_relaxed),
            originalProcess.load(std::memory_order_acquire) != nullptr,
            wrapperMatched.load(std::memory_order_acquire), engineMatched.load(std::memory_order_acquire)};
}
