/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PREPARATION_H
#define FPLINUX_PREPARATION_H

#include "fplinux-font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum fplinux_preparation_stage {
	FPLINUX_PREPARATION_WAITING,
	FPLINUX_PREPARATION_READING,
	FPLINUX_PREPARATION_EXTRACTING,
	FPLINUX_PREPARATION_READY,
	FPLINUX_PREPARATION_ERROR,
	FPLINUX_PREPARATION_CANCELLED,
};

struct fplinux_preparation_status {
	enum fplinux_preparation_stage stage;
	uint64_t completed;
	uint64_t total;
};

struct fplinux_preparation_surface {
	uint16_t *pixels;
	size_t stride_bytes;
	unsigned int width;
	unsigned int height;
};

bool fplinux_preparation_parse_stage(const char *name,
				     enum fplinux_preparation_stage *stage);
/* A missing file means waiting; invalid input leaves status unchanged. */
bool fplinux_preparation_read(const char *path,
			      struct fplinux_preparation_status *status);
bool fplinux_preparation_write(const char *path,
			       const struct fplinux_preparation_status *status);
bool fplinux_preparation_is_busy(enum fplinux_preparation_stage stage);
void fplinux_preparation_render(
	const struct fplinux_preparation_surface *surface,
	const struct fplinux_font *font,
	const struct fplinux_preparation_status *status, unsigned int phase);

#endif
