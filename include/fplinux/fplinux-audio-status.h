/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_AUDIO_STATUS_H
#define FPLINUX_AUDIO_STATUS_H

#include "fplinux-audio-protocol.h"

#include <stdbool.h>

/* The status socket sends one snapshot after accept, then closes. */
struct fplinux_audio_status {
	unsigned int media_volume;
	unsigned int alarm_volume;
	uint32_t rate;
	enum fplinux_audio_route route;
	enum fplinux_audio_route applied_route;
	bool speaker;
	bool headphones;
	bool jack;
	bool plugged;
	bool fm;
	bool fm_active;
	bool clock;
	bool foreground;
	bool pcm;
	uint64_t clipped;
	int error;
};

int fplinux_audio_status_send(int fd,
			      const struct fplinux_audio_status *status);
/* Nonblocking. EPROTO denotes a missing or invalid snapshot. */
int fplinux_audio_status_receive(int fd, struct fplinux_audio_status *status);

#endif
