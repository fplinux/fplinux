// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_TEST_QUAKE_STATS_CVAR_H
#define FPLINUX_TEST_QUAKE_STATS_CVAR_H

#include <stdint.h>

typedef struct cvar_s {
	const char *name;
	const char *string;
	uint32_t flags;
	float value;
	void (*callback)(struct cvar_s *variable);
} cvar_t;

void Cvar_RegisterVariable(cvar_t *variable);

#endif
