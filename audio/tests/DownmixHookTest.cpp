// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <dlfcn.h>
#include "../DownmixHook.h"
int main(int argc, char** argv) {
    assert(argc == 2);
    void* vendor = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(vendor);
    assert(tetris_audio_hook_initialize(vendor));
    assert(!tetris_audio_hook_compatible());
    auto invoke = reinterpret_cast<int (*)(void*, size_t)>(dlsym(vendor, "invokeVendor"));
    assert(invoke);
    unsigned char sample = 0;
    auto count = tetris_audio_hook_stats().original;
    assert(invoke(&sample, 1) == 37);
    assert(sample == 0x5a && tetris_audio_hook_stats().original == count + 1);
    // An unaudited vendor function still delegates even inside a stereo scope.
    TetrisHookScope scope{};
    assert(tetris_audio_hook_bind(&sample) == 0);
    tetris_audio_hook_begin(&scope, &sample, 1);
    sample = 0;
    assert(invoke(&sample, 1) == 37);
    assert(sample == 0x5a && tetris_audio_hook_stats().original == count + 2);
    tetris_audio_hook_end(&scope);
    auto mixer = reinterpret_cast<int (*)(void*, unsigned int, void*)>(dlsym(vendor, "invokeMixer"));
    assert(mixer);
    sample = 0;
    assert(mixer(&sample, 1, &sample) == 37 && sample == 0x5a);
    assert(tetris_audio_hook_stats().original == count + 3);
    puts("ELF downmix interposition and unknown-vendor fallback passed");
}
