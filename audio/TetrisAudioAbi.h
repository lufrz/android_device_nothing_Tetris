// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <linux/ioctl.h>
#include <linux/types.h>

#define TETRIS_AUDIO_RECEIVER_MIN_DB (-10)
#define TETRIS_AUDIO_RECEIVER_MAX_DB 6
#define TETRIS_AUDIO_RECEIVER_DEFAULT_DB 0

struct tetris_audio_caps {
    __u32 version;
    __u32 modes;
    __u32 receiver_gain;
    __u32 reserved;
};
#define TETRIS_AUDIO_GET_CAPS _IOR('T', 0, struct tetris_audio_caps)
#define TETRIS_AUDIO_SET_MODE _IOW('T', 1, __u32)
#define TETRIS_AUDIO_GET_STATE _IOR('T', 2, __u32)
// Optional signed dB control; older ABI-v1 kernels return -ENOTTY.
#define TETRIS_AUDIO_SET_GAIN _IOW('T', 3, __s32)
#define TETRIS_AUDIO_GET_GAIN _IOR('T', 4, __s32)

/* Optional, descriptor-bound PCM path. Independent of the routing ABI above. */
#define TETRIS_AUDIO_SPLIT_VERSION 1
#define TETRIS_AUDIO_SPLIT_FORMAT_S32_LE 1
struct tetris_audio_split_caps {
	__u32 version;
	__u32 card;
	__u32 device;
	__u32 rate;
	__u32 channels;
	__u32 format;
	__u32 reserved[2];
};
#define TETRIS_AUDIO_GET_SPLIT_CAPS _IOR('T', 5, struct tetris_audio_split_caps)
/* Bind a configured playback fd; -1 unbinds it while the receiver is off. */
#define TETRIS_AUDIO_BIND_SPLIT_PCM _IOW('T', 6, __s32)
