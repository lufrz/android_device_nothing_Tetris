// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

using TetrisOriginalDownmix = int (*)(void*, void*, size_t);
using TetrisOriginalMixerWrite = int (*)(void*, unsigned int, void*);

// Caller-owned scope: never retained after the synchronous write returns.
struct TetrisHookScope {
    TetrisHookScope* previous = nullptr;
    TetrisHookScope* observer = nullptr;
    void* handler = nullptr;
    uint64_t generation = 0;
    bool worker = false;
    void* observedHandler = nullptr;
    bool ambiguous = false;
};
struct TetrisHookStats {
    uint64_t synchronous;
    uint64_t worker;
    uint64_t original;
    uint64_t normal;
    uint64_t fast;
    uint64_t dsp;
    uint64_t speakerProtection;
    uint64_t unknown;
};
extern "C" bool tetris_audio_hook_initialize(void* vendor_handle);
extern "C" bool tetris_audio_hook_compatible();
extern "C" uint64_t tetris_audio_hook_bind(void* handler);
extern "C" void tetris_audio_hook_clear();
extern "C" void tetris_audio_hook_begin(TetrisHookScope*, void* handler, uint64_t generation);
extern "C" void tetris_audio_hook_end(TetrisHookScope*);
extern "C" TetrisHookStats tetris_audio_hook_stats();
// Read-only probes may inspect only the currently bound, live write scope.
bool tetris_audio_hook_authorized_scope();
