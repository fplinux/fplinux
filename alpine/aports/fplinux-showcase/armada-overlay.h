// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_ARMADA_OVERLAY_H
#define FPLINUX_ARMADA_OVERLAY_H

#include "armada-scene.h"

void armada_overlay_draw(unsigned int width, unsigned int height,
			 uint16_t *pixels, const struct fplinux_font *font,
			 uint32_t frame, const struct armada_metrics *metrics);
void armada_overlay_draw_stride(unsigned int width, unsigned int height,
				uint16_t *pixels, size_t stride,
				const struct fplinux_font *font, uint32_t frame,
				const struct armada_metrics *metrics);

#endif
