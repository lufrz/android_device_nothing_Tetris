// SPDX-License-Identifier: Apache-2.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../AudioMode.h"
#include <cassert>
#include <cstdio>
using namespace tetris::audio;
int main() {
    assert(parseMode("off") == Mode::Off);
    assert(parseMode("dual_mono") == Mode::DualMono);
    assert(parseMode("stereo") == Mode::Stereo);
    for (auto value : {"", "1", "STEREO", "stereo;off", "stereo ", "dual-mono"}) {
        assert(!parseMode(value).has_value());
    }
    PlaybackState allowed{true, true, true, true, true, false, false, true};
    assert(eligible(allowed));
    for (auto field : {&PlaybackState::normalMode, &PlaybackState::speakerOnly,
                      &PlaybackState::pcmStereo, &PlaybackState::mediaOnly,
                      &PlaybackState::exclusiveOutput, &PlaybackState::primed}) {
        auto blocked = allowed;
        blocked.*field = false;
        assert(!eligible(blocked));
    }
    for (auto field : {&PlaybackState::captureOpen, &PlaybackState::preempted}) {
        auto blocked = allowed;
        blocked.*field = true;
        assert(!eligible(blocked));
    }
    assert(!eligible({}));
    puts("Tetris audio mode checks passed");
}
