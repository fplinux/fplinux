/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_GPU_OUTPUT_H
#define FPLINUX_GPU_OUTPUT_H

#include "fplinux-gpu.h"

struct fplinux_drm_session;
struct fplinux_gpu_output;

/* Owns the GPU; the borrowed session must outlive this output. */
struct fplinux_gpu_output *
fplinux_gpu_output_open(struct fplinux_drm_session *session,
			enum fplinux_gpu_api api, unsigned int source_width,
			unsigned int source_height, bool clockwise, char *error,
			size_t error_size);
/* Borrowed context for scene resources; do not close it separately. */
struct fplinux_gpu *
fplinux_gpu_output_gpu(const struct fplinux_gpu_output *output);

/*
 * Waits for the previous presentation before writing the shared display buffer,
 * then resolves an RGBA8 texture and waits for the GPU writes to complete.
 * Caller scene resources can be rendered independently before this call.
 */
bool fplinux_gpu_output_render(struct fplinux_gpu_output *output,
			       unsigned int texture);
/* No VT dispatch is permitted from render through present, including access. */
bool fplinux_gpu_output_begin_cpu(struct fplinux_gpu_output *output,
				  void **pixels, size_t *stride);
bool fplinux_gpu_output_end_cpu(struct fplinux_gpu_output *output);
/* Queues display transfer; render, VT changes or close report completion errors. */
bool fplinux_gpu_output_present(struct fplinux_gpu_output *output);
const char *fplinux_gpu_output_error(const struct fplinux_gpu_output *output);
/*
 * Delete caller scene resources first; this also destroys the owned context.
 * Drains the final presentation and always releases the output.
 * False reports a completion or cleanup error through errno.
 */
bool fplinux_gpu_output_close(struct fplinux_gpu_output *output);

#endif
