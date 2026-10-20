/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_DISPLAY_TEST_GPU_H
#define FPLINUX_DISPLAY_TEST_GPU_H

#include "display-test-pattern.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fplinux_drm_session;
struct fplinux_display_test_gpu;

struct fplinux_display_test_gpu *
fplinux_display_test_gpu_open(struct fplinux_drm_session *session, char *error,
			      size_t error_size);
bool fplinux_display_test_gpu_draw(struct fplinux_display_test_gpu *renderer,
				   enum fplinux_display_test_pattern pattern,
				   uint32_t frame_id);
bool fplinux_display_test_gpu_present(struct fplinux_display_test_gpu *renderer);
bool fplinux_display_test_gpu_verify(struct fplinux_display_test_gpu *renderer,
				     enum fplinux_display_test_pattern pattern,
				     uint32_t frame_id);
const char *
fplinux_display_test_gpu_error(const struct fplinux_display_test_gpu *renderer);
bool fplinux_display_test_gpu_close(struct fplinux_display_test_gpu *renderer);

#endif
