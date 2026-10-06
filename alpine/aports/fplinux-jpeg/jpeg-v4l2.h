/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_JPEG_V4L2_H
#define FPLINUX_JPEG_V4L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum fplinux_jpeg_operation {
	FPLINUX_JPEG_DECODE,
	FPLINUX_JPEG_ENCODE,
	FPLINUX_JPEG_SCALE,
};

struct fplinux_jpeg_v4l2_request {
	const char *device_path;
	enum fplinux_jpeg_operation operation;
	unsigned int scale;
	unsigned int width;
	unsigned int height;
	unsigned int quality;
	unsigned int repeat;
	bool timing;
};

struct fplinux_jpeg_v4l2_result {
	uint8_t *data;
	size_t length;
	uint32_t width;
	uint32_t height;
	uint32_t stride[2];
	uint32_t bytesused[2];
	unsigned int vsub;
	unsigned int quality;
};

struct fplinux_jpeg_v4l2_timing {
	struct timespec prepare_done;
	uint64_t queue_ns;
	uint64_t wait_ns;
	uint64_t warm_queue_ns;
	uint64_t warm_wait_ns;
	uint64_t loop_ns;
	uint64_t warm_loop_ns;
};

/*
 * Repeats reuse configured queues. On success the caller owns result->data;
 * streams, mappings and the device have already been released.
 */
int fplinux_jpeg_v4l2_run(const struct fplinux_jpeg_v4l2_request *request,
			  const uint8_t *input, size_t input_length,
			  struct fplinux_jpeg_v4l2_result *result,
			  struct fplinux_jpeg_v4l2_timing *timing, char *error,
			  size_t error_size);

#endif
