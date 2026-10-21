// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <optional>
#include <string_view>

namespace tetris::audio {
enum class Mode : unsigned { Off = 0, DualMono = 1, Stereo = 2 };
inline const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::DualMono: return "dual_mono";
        case Mode::Stereo: return "stereo";
        default: return "off";
    }
}
inline std::optional<Mode> parseMode(std::string_view value) {
    if (value == "off") return Mode::Off;
    if (value == "dual_mono") return Mode::DualMono;
    if (value == "stereo") return Mode::Stereo;
    return std::nullopt;
}
struct PlaybackState {
    bool normalMode = false;
    bool speakerOnly = false;
    bool pcmStereo = false;
    bool mediaOnly = false;
    bool exclusiveOutput = false;
    bool captureOpen = false;
    bool preempted = false;
    bool primed = false;
};
inline bool eligible(const PlaybackState& state) {
    return state.normalMode && state.speakerOnly && state.pcmStereo &&
            state.mediaOnly && state.exclusiveOutput && !state.captureOpen &&
            !state.preempted && state.primed;
}
} // namespace tetris::audio
