// SPDX-License-Identifier: Apache-2.0
#pragma once

// Match the Tetris vendor HAL and the device's HIDL compatibility setting.
// This must precede every system/audio.h include, including in host tests.
#if defined(ANDROID_AUDIO_CORE_H) && !defined(SKIP_SPEAKER_LAYOUT_CHANNEL_MASK_FIELD)
#error "Include LegacyAudioHal.h before the platform audio headers"
#endif
#ifndef SKIP_SPEAKER_LAYOUT_CHANNEL_MASK_FIELD
#define SKIP_SPEAKER_LAYOUT_CHANNEL_MASK_FIELD
#endif
#include <hardware/audio.h>
#include <cstddef>

static_assert(sizeof(audio_port_config_device_ext) == 40);
static_assert(sizeof(audio_port_config) == 216);
static_assert(alignof(audio_port_config) == 4);
static_assert(offsetof(audio_port_config, type) == 8);
static_assert(offsetof(audio_port_config, ext) == 176);
static_assert(offsetof(audio_port_config, ext.device.type) == 180);
static_assert(offsetof(audio_port_config, ext.mix.handle) == 180);
