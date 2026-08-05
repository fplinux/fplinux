// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_BOOT_SCREEN_H
#define FPLINUX_BOOT_SCREEN_H

#include <stddef.h>
#include <stdint.h>

#define FPLINUX_BOOT_SCREEN_MAX_STAGES 8u
#define FPLINUX_BOOT_SCREEN_IDENTITY_BYTES 32u
#define FPLINUX_BOOT_SCREEN_STAGE_LABEL_BYTES 32u
#define FPLINUX_BOOT_SCREEN_STATUS_BYTES 96u

enum fplinux_boot_screen_status {
	FPLINUX_BOOT_SCREEN_PENDING = 0,
	FPLINUX_BOOT_SCREEN_ACTIVE,
	FPLINUX_BOOT_SCREEN_DONE,
	FPLINUX_BOOT_SCREEN_FAILED,
};

typedef void (*fplinux_boot_screen_fill_rect_fn)(void *context, uint32_t x,
						 uint32_t y, uint32_t width,
						 uint32_t height,
						 uint16_t rgb565);
typedef void (*fplinux_boot_screen_present_fn)(void *context);

struct fplinux_boot_screen_canvas {
	uint32_t width;
	uint32_t height;
	void *context;
	fplinux_boot_screen_fill_rect_fn fill_rect;
	fplinux_boot_screen_present_fn present;
};

struct fplinux_boot_screen_identity {
	const char *brand;
	const char *variant;
	const char *model;
	const char *mode;
};

/*
 * Fixed-size state for freestanding callers.  Text passed to init and update
 * functions is copied, so callers do not need to retain temporary buffers.
 */
struct fplinux_boot_screen {
	struct fplinux_boot_screen_canvas canvas;
	char brand[FPLINUX_BOOT_SCREEN_IDENTITY_BYTES];
	char variant[FPLINUX_BOOT_SCREEN_IDENTITY_BYTES];
	char model[FPLINUX_BOOT_SCREEN_IDENTITY_BYTES];
	char mode[FPLINUX_BOOT_SCREEN_IDENTITY_BYTES];
	char stage_labels[FPLINUX_BOOT_SCREEN_MAX_STAGES]
			 [FPLINUX_BOOT_SCREEN_STAGE_LABEL_BYTES];
	char status_text[FPLINUX_BOOT_SCREEN_STATUS_BYTES];
	char error_detail[FPLINUX_BOOT_SCREEN_STATUS_BYTES];
	enum fplinux_boot_screen_status
	    stage_status[FPLINUX_BOOT_SCREEN_MAX_STAGES];
	enum fplinux_boot_screen_status status_state;
	size_t stage_count;
	size_t current_stage;
	uint32_t error_code;
	uint8_t initialized;
	uint8_t has_error;
};

int fplinux_boot_screen_init(
    struct fplinux_boot_screen *screen,
    const struct fplinux_boot_screen_canvas *canvas,
    const struct fplinux_boot_screen_identity *identity,
    const char *const *stage_labels, size_t stage_count);

int fplinux_boot_screen_set_stage(struct fplinux_boot_screen *screen,
				  size_t stage_index,
				  enum fplinux_boot_screen_status status);

int fplinux_boot_screen_set_checkpoint(struct fplinux_boot_screen *screen,
				       const char *text,
				       enum fplinux_boot_screen_status status);

void fplinux_boot_screen_fail(struct fplinux_boot_screen *screen,
			      uint32_t error_code, const char *detail);

void fplinux_boot_screen_render(struct fplinux_boot_screen *screen);

#endif
