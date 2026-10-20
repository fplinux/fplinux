// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_ARMADA_GPU_RENDERER_H
#define FPLINUX_ARMADA_GPU_RENDERER_H

#include "armada-frame.h"

#include <stddef.h>

struct armada_gpu_renderer;
struct fplinux_drm_session;
struct fplinux_gpu_output;

/* Failed setup reports whether software fallback can acquire resources. */
struct armada_gpu_renderer *
armada_gpu_renderer_create(struct fplinux_drm_session *display,
			   unsigned int width, unsigned int height,
			   bool *cleanup_complete, char *error,
			   size_t error_size);
bool armada_gpu_renderer_render(struct armada_gpu_renderer *renderer,
				const struct armada_frame *frame);
struct fplinux_gpu_output *
armada_gpu_renderer_output(const struct armada_gpu_renderer *renderer);
const char *
armada_gpu_renderer_name(const struct armada_gpu_renderer *renderer);
const char *
armada_gpu_renderer_error(const struct armada_gpu_renderer *renderer);
bool armada_gpu_renderer_destroy(struct armada_gpu_renderer *renderer);

#endif
