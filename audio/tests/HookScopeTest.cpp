// SPDX-License-Identifier: Apache-2.0
// Exercise audited-function behavior with callback mocks, never a device or PCM.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <thread>
#include "../DownmixHook.cpp"

namespace {
uintptr_t handlerA = 0, handlerB = 0;
std::mutex pauseMutex;
std::condition_variable pauseCv;
bool pauseCallback = false, entered = false, resume = false;
bool nestedCallback = false;
int stockDownmix(void*, void* buffer, size_t bytes) {
    if (bytes == sizeof(int16_t) * 2) {
        auto* stereo = static_cast<int16_t*>(buffer);
        const auto mono = static_cast<int16_t>((static_cast<int>(stereo[0]) + stereo[1]) / 2);
        stereo[0] = stereo[1] = mono;
    } else if (bytes) {
        *static_cast<unsigned char*>(buffer) = 0x5a;
    }
    return 37;
}
int mixerWrite(void* buffer, unsigned bytes, void* handler) {
    if (pauseCallback) {
        std::unique_lock lock(pauseMutex);
        entered = true;
        pauseCv.notify_all();
        pauseCv.wait(lock, [] { return resume; });
    }
    if (nestedCallback) {
        nestedCallback = false;
        unsigned char other = 0;
        assert(tetris_audio_vendor_mixer_write(&other, 1, &handlerB) == 37);
        assert(other == 0x5a);
    }
    return tetris_audio_vendor_downmix(handler, buffer, bytes);
}
int worker(void* handler) {
    int result = -1;
    std::thread task([&] {
        unsigned char sample = 0;
        result = tetris_audio_vendor_mixer_write(&sample, 1, handler);
        assert(sample == (result == 37 ? 0x5a : 0));
    });
    task.join();
    return result;
}
}
int main() {
    original.store(stockDownmix);
    originalMixer.store(mixerWrite);
    compatible.store(true); // Fingerprint rejection is separately tested with real ELF objects.
    TetrisHookScope first{};
    tetris_audio_hook_begin(&first, nullptr, 0);
    unsigned char sample = 0;
    assert(tetris_audio_vendor_mixer_write(&sample, 1, &handlerA) == 37);
    assert(first.observedHandler == &handlerA && !first.ambiguous);
    tetris_audio_hook_end(&first);
    auto generation = tetris_audio_hook_bind(first.observedHandler);
    assert(generation);

    // Same backend can move from the caller to a mixer worker without downmixing.
    TetrisHookScope write{};
    tetris_audio_hook_begin(&write, &handlerA, generation);
    sample = 0;
    assert(tetris_audio_vendor_downmix(&handlerA, &sample, 1) == 0 && sample == 0);
    tetris_audio_hook_end(&write);
    assert(worker(&handlerA) == 0);
    assert(worker(&handlerB) == 37);
    // An unrelated thread invoking downmix directly never inherits the binding.
    assert(tetris_audio_vendor_downmix(&handlerA, &sample, 1) == 37);
    // Nested callbacks restore the outer scope, but a different handler stays stock.
    nestedCallback = true;
    assert(worker(&handlerA) == 0);
    assert(scope == nullptr);

    // A callback paused before conversion cannot reuse a revoked lease, even
    // when a new lease has already bound the very same pointer address.
    pauseCallback = true;
    int pausedResult = -1;
    std::thread paused([&] {
        unsigned char data = 0;
        pausedResult = tetris_audio_vendor_mixer_write(&data, 1, &handlerA);
    });
    {
        std::unique_lock lock(pauseMutex);
        pauseCv.wait(lock, [] { return entered; });
    }
    tetris_audio_hook_clear();
    auto next = tetris_audio_hook_bind(&handlerA);
    assert(next != generation);
    {
        std::lock_guard lock(pauseMutex);
        resume = true;
    }
    pauseCv.notify_all();
    paused.join();
    assert(pausedResult == 37);
    pauseCallback = false;
    assert(worker(&handlerA) == 0);
    tetris_audio_hook_clear();
    assert(worker(&handlerA) == 37);

    // A priming write that reaches more than one backend is not unique.
    TetrisHookScope ambiguous{};
    tetris_audio_hook_begin(&ambiguous, nullptr, 0);
    tetris_audio_vendor_mixer_write(&sample, 1, &handlerA);
    tetris_audio_vendor_mixer_write(&sample, 1, &handlerB);
    assert(ambiguous.ambiguous);
    tetris_audio_hook_end(&ambiguous);
    // Nested caller scopes also restore their observer, with no dangling TLS.
    TetrisHookScope outer{}, inner{};
    tetris_audio_hook_begin(&outer, nullptr, 0);
    tetris_audio_hook_begin(&inner, nullptr, 0);
    tetris_audio_vendor_downmix(&handlerB, &sample, 1);
    tetris_audio_hook_end(&inner);
    tetris_audio_vendor_downmix(&handlerA, &sample, 1);
    assert(inner.observedHandler == &handlerB && outer.observedHandler == &handlerA);
    tetris_audio_hook_end(&outer);
    assert(scope == nullptr);
    auto stats = tetris_audio_hook_stats();
    assert(stats.synchronous == 1 && stats.worker == 3 && stats.original >= 8);
    // Only authorized conversions inspect a backend's primary vptr. Unknown
    // classes remain an explicit bucket and never affect bypass decisions.
    for (size_t i = 0; i < backendVtables.size(); ++i) {
        backendVtables[i].store(0x1000 + i * 0x100);
    }
    const auto priorUnknown = stats.unknown;
    for (size_t i = 0; i <= backendVtables.size(); ++i) {
        handlerA = 0x1000 + i * 0x100;
        const auto token = tetris_audio_hook_bind(&handlerA);
        TetrisHookScope classified{};
        tetris_audio_hook_begin(&classified, &handlerA, token);
        assert(tetris_audio_vendor_downmix(&handlerA, &sample, 1) == 0);
        tetris_audio_hook_end(&classified);
    }
    stats = tetris_audio_hook_stats();
    assert(stats.normal == 1 && stats.fast == 1 && stats.dsp == 1 && stats.speakerProtection == 1);
    assert(stats.unknown == priorUnknown + 1);
    // A foreign pointer is delegated without inspecting its memory.
    assert(tetris_audio_vendor_downmix(reinterpret_cast<void*>(1), &sample, 1) == 37);
    assert(tetris_audio_hook_stats().unknown == stats.unknown);
    // Explicit left-only/right-only PCM: authorised paths preserve separation;
    // wrong or revoked owners run the real averaging behavior of the mock.
    const std::array<int16_t, 2> mono{600, 600};
    for (const std::array<int16_t, 2> input : {
            std::array<int16_t, 2>{1200, 0}, std::array<int16_t, 2>{0, 1200}}) {
        const auto token = tetris_audio_hook_bind(&handlerA);
        auto pcm = input;
        TetrisHookScope synchronous{};
        tetris_audio_hook_begin(&synchronous, &handlerA, token);
        assert(tetris_audio_vendor_downmix(&handlerA, pcm.data(), sizeof(pcm)) == 0);
        tetris_audio_hook_end(&synchronous);
        assert(pcm == input);

        std::thread authorised([&] {
            pcm = input;
            assert(tetris_audio_vendor_mixer_write(pcm.data(), sizeof(pcm), &handlerA) == 0);
            assert(pcm == input);
        });
        authorised.join();
        std::thread wrongOwner([&] {
            pcm = input;
            assert(tetris_audio_vendor_mixer_write(pcm.data(), sizeof(pcm), &handlerB) == 37);
            assert(pcm == mono);
        });
        wrongOwner.join();
        tetris_audio_hook_clear();
        std::thread revoked([&] {
            pcm = input;
            assert(tetris_audio_vendor_mixer_write(pcm.data(), sizeof(pcm), &handlerA) == 37);
            assert(pcm == mono);
        });
        revoked.join();
    }
    puts("Scoped vendor worker, stereo PCM, revocation, handler identity and nesting checks passed");
}
