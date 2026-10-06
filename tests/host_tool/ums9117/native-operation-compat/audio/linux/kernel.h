/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIO_PROFILE_HOST_KERNEL_H
#define FPLINUX_AUDIO_PROFILE_HOST_KERNEL_H

#define S16_MAX 32767
#define abs(value)                             \
	({                                     \
		__typeof__(value) v = (value); \
		v < 0 ? -v : v;                \
	})

#endif
