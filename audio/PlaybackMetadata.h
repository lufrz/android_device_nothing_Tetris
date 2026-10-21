// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "LegacyAudioHal.h"
#include <cstring>
#include <limits>

namespace tetris::audio {

// Tetris's proprietary 7.1 adapter uses android.hardware.audio@7.1-util-v34.
// Its sourceMetadataToHalV7 copies and advances by 0x110 bytes per track.
// Current platform headers append codec_provenance; the speaker-layout
// compatibility macro does not remove it, so platform array indexing is unsafe.
struct LegacyPlaybackTrackMetadataV7 {
    playback_track_metadata base;
    audio_channel_mask_t channelMask;
    char tags[256];
};
static_assert(sizeof(audio_usage_t) == 4);
static_assert(sizeof(playback_track_metadata) == 12);
static_assert(offsetof(playback_track_metadata, usage) == 0);
static_assert(sizeof(LegacyPlaybackTrackMetadataV7) == 272);
static_assert(alignof(LegacyPlaybackTrackMetadataV7) == 4);
static_assert(offsetof(LegacyPlaybackTrackMetadataV7, base) == 0);
static_assert(offsetof(LegacyPlaybackTrackMetadataV7, channelMask) == 12);
static_assert(offsetof(LegacyPlaybackTrackMetadataV7, tags) == 16);

namespace detail {
inline bool mediaOnlyTracks(const void* tracks, size_t count, size_t stride) {
    if (!tracks || !count || count > std::numeric_limits<size_t>::max() / stride) return false;
    const auto* bytes = static_cast<const unsigned char*>(tracks);
    for (size_t i = 0; i < count; ++i) {
        audio_usage_t usage;
        std::memcpy(&usage, bytes + i * stride, sizeof(usage));
        if (usage != AUDIO_USAGE_MEDIA && usage != AUDIO_USAGE_GAME) return false;
    }
    return true;
}
} // namespace detail

inline bool mediaOnlyMetadata(const source_metadata* metadata) {
    return metadata && detail::mediaOnlyTracks(metadata->tracks, metadata->track_count,
                                               sizeof(playback_track_metadata));
}
inline bool mediaOnlyMetadata(const source_metadata_v7* metadata) {
    return metadata && detail::mediaOnlyTracks(metadata->tracks, metadata->track_count,
                                               sizeof(LegacyPlaybackTrackMetadataV7));
}
} // namespace tetris::audio
