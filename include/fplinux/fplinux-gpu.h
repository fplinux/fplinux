/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_GPU_H
#define FPLINUX_GPU_H

#include <stdbool.h>
#include <stddef.h>

struct fplinux_gpu;

enum fplinux_gpu_api {
	FPLINUX_GPU_GLES3,
	FPLINUX_GPU_OPENGL2,
};

/* NULL device selects the unique supported hardware render node. */
struct fplinux_gpu *fplinux_gpu_open(enum fplinux_gpu_api api,
				     unsigned int width, unsigned int height,
				     const char *device, char *error,
				     size_t error_size);
void *fplinux_gpu_get_proc(struct fplinux_gpu *gpu, const char *name);
const char *fplinux_gpu_device(const struct fplinux_gpu *gpu);
const char *fplinux_gpu_renderer(const struct fplinux_gpu *gpu);
const char *fplinux_gpu_version(const struct fplinux_gpu *gpu);
void fplinux_gpu_close(struct fplinux_gpu *gpu);

#endif
