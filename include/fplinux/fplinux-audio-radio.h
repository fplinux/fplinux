/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_AUDIO_RADIO_H
#define FPLINUX_AUDIO_RADIO_H

#define FPLINUX_AUDIO_FM_LOW_100KHZ 875U
#define FPLINUX_AUDIO_FM_HIGH_100KHZ 1080U

/*
 * One request and one reply per lease. SCAN reserves the tuner silently,
 * PLAY waits for the service to apply the FM route. An active clock may
 * interrupt FM; playback resumes at the original station. Close releases FM.
 */
enum fplinux_audio_radio_type {
	FPLINUX_AUDIO_RADIO_SCAN = 1,
	FPLINUX_AUDIO_RADIO_PLAY = 2, /* frequency in 100 kHz units, u16le */
	FPLINUX_AUDIO_RADIO_READY = 3,
	FPLINUX_AUDIO_RADIO_ERROR = 4, /* positive errno, u32le */
};

enum fplinux_audio_radio_direction {
	FPLINUX_AUDIO_RADIO_REQUEST,
	FPLINUX_AUDIO_RADIO_REPLY,
};

struct fplinux_audio_radio {
	enum fplinux_audio_radio_type type;
	union {
		unsigned int frequency;
		int error;
	};
};

int fplinux_audio_radio_send(int fd, const struct fplinux_audio_radio *message);
/* Uses the fd's blocking mode. Returns 1, 0 on EOF, or -1 with errno. */
int fplinux_audio_radio_receive(int fd,
				enum fplinux_audio_radio_direction direction,
				struct fplinux_audio_radio *message);

#endif
