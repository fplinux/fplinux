// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_ARMADA_RENDERER_H
#define FPLINUX_ARMADA_RENDERER_H

#include "armada-frame.h"
#include "armada-scene.h"

struct armada_renderer;

struct armada_renderer *armada_renderer_create(unsigned int width,
					       unsigned int height,
					       uint16_t *pixels,
					       const struct fplinux_font *font);
void armada_renderer_destroy(struct armada_renderer *renderer);
void armada_renderer_render(struct armada_renderer *renderer,
			    const struct armada_frame *frame,
			    const struct armada_metrics *metrics);

#endif
