// SPDX-License-Identifier: Apache-2.0
// Synthetic ARSI buffers and handlers only; no vendor DSP, device or recording.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdio>
#include "../DownmixHook.cpp"
#include "../BessoundProbe.cpp"

namespace {
int firstHandler, otherHandler;
int expectedResult = 0, forwarded = 0;
bool touchBuffers = false, downmix = true, revokeInside = false;
void* expectedOpaque2 = reinterpret_cast<void*>(0x1234);
void* expectedOpaque3 = reinterpret_cast<void*>(0x5678);
struct Buffer {
    uint32_t capacity;
    uint32_t bytes;
    int32_t* data;
};
static_assert(offsetof(Buffer, bytes) == 4 && offsetof(Buffer, data) == 8);
int mockProcess(void* input, void* output, void* second, void* third, void*) {
    assert(second == expectedOpaque2 && third == expectedOpaque3);
    ++forwarded;
    if (touchBuffers) {
        auto& in = *static_cast<Buffer*>(input);
        auto& out = *static_cast<Buffer*>(output);
        assert(out.capacity >= in.bytes);
        for (size_t frame = 0; frame < in.bytes / 8; ++frame) {
            if (downmix) {
                out.data[frame * 2] = out.data[frame * 2 + 1] =
                        (in.data[frame * 2] >> 1) + (in.data[frame * 2 + 1] >> 1);
            } else {
                out.data[frame * 2] = in.data[frame * 2];
                out.data[frame * 2 + 1] = in.data[frame * 2 + 1];
            }
        }
        out.bytes = in.bytes;
        in.bytes = 0; // Vendor processing may consume its input descriptor.
    }
    if (revokeInside) tetris_audio_hook_clear();
    return expectedResult;
}
int invoke(void* in, void* out, void* state) {
    return audioloud_arsi_process_dl_buf(in, out, expectedOpaque2, expectedOpaque3, state);
}
template <typename T, size_t N> void put(std::array<uint8_t, N>& data, size_t offset, T value) {
    assert(offset + sizeof(value) <= data.size());
    std::memcpy(data.data() + offset, &value, sizeof(value));
}
}

int main() {
    using namespace bessound_probe;
    // Reading diagnostics must neither resolve nor load the vendor effect.
    auto counters = tetris_bessound_probe_stats();
    assert(counters.totalCalls == 0 && counters.unscopedCalls == 0);
    assert(!counters.originalResolved && !counters.wrapperMatched && !counters.engineMatched);
    // With no vendor library in this host process, resolution must fail cleanly
    // without inspecting invalid opaque pointers. Install a mock only afterward.
    auto* invalid = reinterpret_cast<void*>(1);
    assert(invoke(invalid, invalid, invalid) == -ENODEV);
    counters = tetris_bessound_probe_stats();
    assert(counters.totalCalls == 1 && counters.unscopedCalls == 1);
    assert(!counters.originalResolved && counters.scopedCalls == 0);
    originalProcess.store(mockProcess);
    compatible.store(true);
    auto generation = tetris_audio_hook_bind(&firstHandler);
    assert(generation);
    expectedResult = 37;
    assert(!tetris_audio_hook_authorized_scope());
    assert(invoke(invalid, invalid, invalid) == 37); // Never inspect outside a scope.
    assert(tetris_bessound_probe_stats().scopedCalls == 0);
    counters = tetris_bessound_probe_stats();
    assert(counters.totalCalls == 2 && counters.unscopedCalls == 2);
    assert(counters.originalResolved && !counters.wrapperMatched && !counters.engineMatched);

    TetrisHookScope scopeWrong{};
    tetris_audio_hook_begin(&scopeWrong, &otherHandler, generation);
    assert(!tetris_audio_hook_authorized_scope());
    assert(invoke(invalid, invalid, invalid) == 37);
    tetris_audio_hook_end(&scopeWrong);
    assert(tetris_bessound_probe_stats().scopedCalls == 0);
    assert(tetris_bessound_probe_stats().unscopedCalls == 3);

    uintptr_t hostBase = 0;
    assert(!matchesLibrary(reinterpret_cast<void*>(mockProcess), kWrapper, hostBase));
    assert(!matchesLibrary(reinterpret_cast<void*>(mockProcess), kEngine, hostBase));
    TetrisHookScope scopeValid{};
    tetris_audio_hook_begin(&scopeValid, &firstHandler, generation);
    assert(tetris_audio_hook_authorized_scope());
    audited.store(false);
    assert(invoke(invalid, invalid, invalid) == 37); // Unknown ABI still forwards exactly.
    assert(tetris_bessound_probe_stats().unknownCalls == 1);
    assert(tetris_bessound_probe_stats().inputFrames == 0);
    assert(tetris_bessound_probe_stats().totalCalls == 4);
    assert(tetris_bessound_probe_stats().unscopedCalls == 3);

    alignas(16) std::array<uint8_t, 0x3000> handler{};
    engineBase = 0x100000;
    put(handler, 0x08, engineBase + 0x13970);
    put(handler, 0x10, engineBase + 0x13840);
    put(handler, 0x28, engineBase + 0x13a50);
    put(handler, 0x50, reinterpret_cast<uintptr_t>(handler.data()) + 0x640);
    put(handler, 0x30, reinterpret_cast<uintptr_t>(handler.data()) + 0xc84);
    put(handler, 0x38, uint32_t{2});
    put(handler, 0x40, uint32_t{1});
    put(handler, 0xc84, uint32_t{0});
    put(handler, 0xc8c, uint32_t{1});
    const auto snapshot = handler;
    audited.store(true);
    touchBuffers = true;
    expectedResult = 0;
    std::array<int32_t, 160> input{}, output{}; // 80 frames, probe must cap at64.
    for (size_t frame = 0; frame < 80; ++frame) input[frame * 2] = 100;
    Buffer in{sizeof(input), sizeof(input), input.data()};
    Buffer out{sizeof(output), 0, output.data()};
    assert(invoke(&in, &out, handler.data()) == 0);
    assert(handler == snapshot); // Probe never writes a DSP field or mode.
    counters = tetris_bessound_probe_stats();
    assert(counters.monoCalls == 1 && counters.stereoCalls == 0);
    assert(counters.inputFrames == 64 && counters.inputDifferent == 64);
    assert(counters.outputFrames == 64 && counters.outputDifferent == 0);

    put(handler, 0xc84, uint32_t{1});
    put(handler, 0xc8c, uint32_t{0});
    downmix = false;
    in.bytes = sizeof(input);
    assert(invoke(&in, &out, handler.data()) == 0);
    counters = tetris_bessound_probe_stats();
    assert(counters.stereoCalls == 1 && counters.outputDifferent == 64);

    // Unsupported format/pointer layout is forwarded without reading PCM.
    touchBuffers = false;
    put(handler, 0x40, uint32_t{0});
    assert(invoke(invalid, invalid, handler.data()) == 0);
    assert(tetris_bessound_probe_stats().unknownCalls == 2);
    put(handler, 0x40, uint32_t{1});
    put(handler, 0x30, uintptr_t{1});
    assert(invoke(invalid, invalid, handler.data()) == 0);
    assert(tetris_bessound_probe_stats().unknownCalls == 3);
    put(handler, 0x30, reinterpret_cast<uintptr_t>(handler.data()) + 0xc84);

    // Failure and revocation preserve vendor return semantics and do not read
    // output samples from a failed or no-longer-authorized processing callback.
    const auto frameCount = tetris_bessound_probe_stats().outputFrames;
    in.bytes = sizeof(input);
    expectedResult = -EBUSY;
    assert(invoke(&in, invalid, handler.data()) == -EBUSY);
    assert(tetris_bessound_probe_stats().outputFrames == frameCount);
    expectedResult = 0;
    revokeInside = true;
    assert(invoke(&in, invalid, handler.data()) == 0);
    assert(!tetris_audio_hook_authorized_scope());
    assert(tetris_bessound_probe_stats().outputFrames == frameCount);
    const auto callCount = tetris_bessound_probe_stats().scopedCalls;
    assert(invoke(invalid, invalid, invalid) == 0);
    assert(tetris_bessound_probe_stats().scopedCalls == callCount);
    tetris_audio_hook_end(&scopeValid);
    assert(forwarded == 10);
    counters = tetris_bessound_probe_stats();
    assert(counters.totalCalls == 11 && counters.unscopedCalls == 4);
    assert(counters.scopedCalls == 7);
    puts("Read-only Bessound scope, ABI rejection, mono/stereo counters and bounded PCM checks passed");
}
