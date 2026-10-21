// SPDX-License-Identifier: Apache-2.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../PlaybackMetadata.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>

using tetris::audio::mediaOnlyMetadata;

int main() {
    assert(!mediaOnlyMetadata(static_cast<const source_metadata*>(nullptr)));
    assert(!mediaOnlyMetadata(static_cast<const source_metadata_v7*>(nullptr)));
    source_metadata legacy{};
    source_metadata_v7 v7{};
    assert(!mediaOnlyMetadata(&legacy));
    assert(!mediaOnlyMetadata(&v7));
    legacy.track_count = v7.track_count = 2;
    assert(!mediaOnlyMetadata(&legacy));
    assert(!mediaOnlyMetadata(&v7));

    // Encode the vendor ABI independently of the helper and platform struct.
    // Allocate exactly two 272-byte records so ASAN catches any final-record
    // overread, including copying a complete current-platform v7 record.
    constexpr size_t vendorStride = 272;
    auto compact = std::make_unique<unsigned char[]>(2 * vendorStride);
    std::memset(compact.get(), 0xa5, 2 * vendorStride);
    std::array<playback_track_metadata, 2> tracks{};
    legacy.tracks = tracks.data();
    v7.tracks = reinterpret_cast<playback_track_metadata_v7*>(compact.get());
    const auto setUsage = [&](size_t index, audio_usage_t usage) {
        tracks[index].usage = usage;
        std::memcpy(compact.get() + index * vendorStride, &usage, sizeof(usage));
    };
    for (auto first : {AUDIO_USAGE_MEDIA, AUDIO_USAGE_GAME}) {
        for (auto second : {AUDIO_USAGE_MEDIA, AUDIO_USAGE_GAME}) {
            setUsage(0, first);
            setUsage(1, second);
            assert(mediaOnlyMetadata(&legacy));
            assert(mediaOnlyMetadata(&v7));
        }
    }

    // A misleading allowed usage at the modern second-record offset must not
    // hide a notification, voice, or unknown usage at the real vendor offset.
    const audio_usage_t decoy = AUDIO_USAGE_MEDIA;
    std::memcpy(compact.get() + 336, &decoy, sizeof(decoy));
    for (auto rejected : {AUDIO_USAGE_NOTIFICATION, AUDIO_USAGE_VOICE_COMMUNICATION,
                          AUDIO_USAGE_UNKNOWN, AUDIO_USAGE_ALARM,
                          AUDIO_USAGE_ASSISTANCE_SONIFICATION}) {
        for (size_t index : {0u, 1u}) {
            setUsage(0, AUDIO_USAGE_MEDIA);
            setUsage(1, AUDIO_USAGE_GAME);
            setUsage(index, rejected);
            assert(!mediaOnlyMetadata(&legacy));
            assert(!mediaOnlyMetadata(&v7));
        }
    }
    setUsage(0, AUDIO_USAGE_GAME);
    setUsage(1, AUDIO_USAGE_MEDIA);
    legacy.track_count = v7.track_count = 1;
    assert(mediaOnlyMetadata(&legacy));
    assert(mediaOnlyMetadata(&v7));
    legacy.track_count = v7.track_count = 0;
    assert(!mediaOnlyMetadata(&legacy));
    assert(!mediaOnlyMetadata(&v7));
    legacy.track_count = v7.track_count = std::numeric_limits<size_t>::max();
    assert(!mediaOnlyMetadata(&legacy));
    assert(!mediaOnlyMetadata(&v7));
    puts("Tetris playback metadata checks passed");
}
