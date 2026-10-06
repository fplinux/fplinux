/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_JPEG_CPU_CODEC_H
#define FPLINUX_JPEG_CPU_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>

#define FPLINUX_JPEG_CPU_MAX_RAW_BYTES (32 * 1024 * 1024)
#define FPLINUX_JPEG_CPU_DQT_BYTES 128

enum fplinux_jpeg_cpu_decode_mode {
	FPLINUX_JPEG_CPU_DECODE_REDUCED,
	FPLINUX_JPEG_CPU_DECODE_BOX,
};

struct fplinux_jpeg_cpu_metric {
	double wall;
	double user;
	double system;
};

struct fplinux_jpeg_cpu_metric_start {
	double wall;
	struct rusage usage;
};

struct fplinux_jpeg_cpu_frame {
	uint8_t *data;
	size_t size;
	unsigned width;
	unsigned height;
	unsigned chroma_width;
	unsigned chroma_height;
	unsigned vsub;
};

struct fplinux_jpeg_cpu_encode_request {
	unsigned width;
	unsigned height;
	unsigned quality;
	unsigned restart;
};

bool fplinux_jpeg_cpu_metric_start(struct fplinux_jpeg_cpu_metric_start *start);
bool fplinux_jpeg_cpu_metric_stop(
	struct fplinux_jpeg_cpu_metric *total,
	const struct fplinux_jpeg_cpu_metric_start *start);
void fplinux_jpeg_cpu_frame_cleanup(struct fplinux_jpeg_cpu_frame *frame);

/* Success transfers the bounded temporary stream to the caller. */
bool fplinux_jpeg_cpu_encode(
	const struct fplinux_jpeg_cpu_encode_request *request,
	const uint8_t *input, const uint8_t *dqt, FILE **result,
	size_t *result_size, struct fplinux_jpeg_cpu_metric *copy_metric,
	struct fplinux_jpeg_cpu_metric *codec_metric, char *error,
	size_t error_size);
bool fplinux_jpeg_cpu_decode(const uint8_t *input, size_t input_size,
			     unsigned scale,
			     enum fplinux_jpeg_cpu_decode_mode mode,
			     struct fplinux_jpeg_cpu_frame *frame,
			     struct fplinux_jpeg_cpu_metric *codec_metric,
			     struct fplinux_jpeg_cpu_metric *convert_metric,
			     char *error, size_t error_size);
bool fplinux_jpeg_cpu_scale(const uint8_t *input, unsigned width,
			    unsigned height,
			    struct fplinux_jpeg_cpu_frame *frame,
			    struct fplinux_jpeg_cpu_metric *filter_metric);

#endif
