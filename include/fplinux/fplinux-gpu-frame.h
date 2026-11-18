/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_GPU_FRAME_H
#define FPLINUX_GPU_FRAME_H

#include "fplinux-gpu.h"

#include <stdint.h>

struct fplinux_gpu_frame;

/*
 * Borrows the current GLES3 GPU context; close the frame before its GPU.
 * Dimensions stay fixed, and RGBA8 scratch storage is reused across reads.
 */
struct fplinux_gpu_frame *
fplinux_gpu_frame_open(struct fplinux_gpu *gpu, unsigned int width,
		       unsigned int height, char *error, size_t error_size);

/*
 * The owning context must be current with no pending GL error. The caller owns
 * the current read framebuffer and provides a complete RGBA8 color attachment
 * covering width x height from (0, 0). No framebuffer binding is changed.
 * Writes tightly packed, top-down RGB565 using nearest channel quantization;
 * alpha is ignored. Capacity is in pixels. On failure pixels are unchanged.
 * Pixel-pack parameters and the pixel-pack buffer binding are restored.
 */
bool fplinux_gpu_frame_read_rgb565(struct fplinux_gpu_frame *frame,
				   uint16_t *pixels, size_t pixel_capacity,
				   char *error, size_t error_size);
void fplinux_gpu_frame_close(struct fplinux_gpu_frame *frame);

#endif
