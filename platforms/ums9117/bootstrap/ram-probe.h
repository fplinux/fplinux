/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_RAM_PROBE_H
#define FPLINUX_UMS9117_RAM_PROBE_H

#include <stdint.h>

/* The words at RAM + 16, 32 and 48 MiB must be accessed with dcache disabled. */
static inline uint32_t
ums9117_probe_ram_size(volatile uint32_t *const probe_words[3])
{
	const uint32_t pattern = 0x12345678;
	uint32_t saved[3];
	uint32_t i, size;

	for (i = 3; i != 0; --i) {
		saved[i - 1] = *probe_words[i - 1];
		*probe_words[i - 1] = pattern + i;
	}
	for (i = 1; i < 4 && *probe_words[i - 1] == pattern + i; ++i)
		;
	size = i << 24;

	/* Undo writes in reverse order: different probe addresses may alias. */
	for (i = 0; i < 3; ++i)
		*probe_words[i] = saved[i];
	return size;
}

#endif
