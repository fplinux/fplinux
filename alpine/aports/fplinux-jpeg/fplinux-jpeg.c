/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE

#include "fplinux-cli.h"
#include "jpeg-v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_JPEG_INPUT_BYTES (1024U * 1024U)
#define MAX_OUTPUT_BYTES (64U * 1024U * 1024U)
#define MAX_RAW_DIMENSION 2048U
#define MAX_REPEAT 4096U
#define ENCODE_QUALITY 85
#define SCALE_LARGE_WIDTH 640U
#define SCALE_LARGE_HEIGHT 480U
#define SCALE_SMALL_WIDTH 320U
#define SCALE_SMALL_HEIGHT 240U

enum cli_option {
	CLI_OPTION_INPUT,
	CLI_OPTION_OUTPUT,
	CLI_OPTION_DEVICE,
	CLI_OPTION_OPERATION,
	CLI_OPTION_SCALE,
	CLI_OPTION_WIDTH,
	CLI_OPTION_HEIGHT,
	CLI_OPTION_QUALITY,
	CLI_OPTION_REPEAT,
	CLI_OPTION_TIMING,
};

struct options {
	const char *input_path;
	const char *output_path;
	const char *device_path;
	enum fplinux_jpeg_operation operation;
	unsigned int scale;
	unsigned int width;
	unsigned int height;
	unsigned int quality;
	unsigned int repeat;
	bool timing;
};

struct timings {
	struct timespec started;
	struct timespec input_done;
	struct fplinux_jpeg_v4l2_timing device;
	struct timespec output_started;
	struct timespec output_done;
	struct rusage usage_started;
	struct rusage usage_done;
};

static bool parse_operation(const char *text,
			    enum fplinux_jpeg_operation *operation)
{
	if (!strcmp(text, "decode"))
		*operation = FPLINUX_JPEG_DECODE;
	else if (!strcmp(text, "encode"))
		*operation = FPLINUX_JPEG_ENCODE;
	else if (!strcmp(text, "scale"))
		*operation = FPLINUX_JPEG_SCALE;
	else
		return false;
	return true;
}

static const char *parse_cli_option(size_t option, const char *value,
				    void *data)
{
	struct options *options = data;

	switch (option) {
	case CLI_OPTION_OPERATION:
		if (!parse_operation(value, &options->operation))
			return "--operation must be decode, encode or scale";
		break;
	case CLI_OPTION_SCALE:
		if (!fplinux_cli_unsigned(value, 1, 4, &options->scale) ||
		    (options->scale != 1 && options->scale != 2 &&
		     options->scale != 4))
			return "--scale must be 1, 2 or 4";
		break;
	case CLI_OPTION_WIDTH:
		if (!fplinux_cli_unsigned(value, 1, MAX_RAW_DIMENSION,
					  &options->width))
			return "--width and --height must be in 1..2048";
		break;
	case CLI_OPTION_HEIGHT:
		if (!fplinux_cli_unsigned(value, 1, MAX_RAW_DIMENSION,
					  &options->height))
			return "--width and --height must be in 1..2048";
		break;
	case CLI_OPTION_QUALITY:
		if (!fplinux_cli_unsigned(value, 1, 100, &options->quality))
			return "--quality must be in 1..100";
		break;
	case CLI_OPTION_REPEAT:
		if (!fplinux_cli_unsigned(value, 1, MAX_REPEAT,
					  &options->repeat))
			return "--repeat must be in 1..4096";
		break;
	case CLI_OPTION_INPUT:
	case CLI_OPTION_OUTPUT:
	case CLI_OPTION_DEVICE:
	case CLI_OPTION_TIMING:
		break;
	}
	return NULL;
}

static enum fplinux_cli_result parse_options(int argc, char **argv,
					     struct options *options)
{
	struct fplinux_cli_option cli_options[] = {
		[CLI_OPTION_INPUT] = {
			.name = "input",
			.metavar = "FILE",
			.help = "read the input image",
			.flags = FPLINUX_CLI_REQUIRED | FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_OUTPUT] = {
			.name = "output",
			.metavar = "FILE",
			.help = "write the output image",
			.flags = FPLINUX_CLI_REQUIRED | FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_DEVICE] = {
			.name = "device",
			.metavar = "PATH",
			.help = "use this V4L2 device (default: discover)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_OPERATION] = {
			.name = "operation",
			.metavar = "decode|encode|scale",
			.help = "select the operation (default: decode)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_SCALE] = {
			.name = "scale",
			.metavar = "1|2|4",
			.help = "set the decode divisor (default: 1)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_WIDTH] = {
			.name = "width",
			.metavar = "N",
			.help = "set raw input width in 1..2048 for encode or scale",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_HEIGHT] = {
			.name = "height",
			.metavar = "N",
			.help = "set raw input height in 1..2048 for encode or scale",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_QUALITY] = {
			.name = "quality",
			.metavar = "N",
			.help = "set encode quality in 1..100 (default: 85)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_REPEAT] = {
			.name = "repeat",
			.metavar = "N",
			.help = "repeat the operation 1..4096 times (default: 1)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_TIMING] = {
			.name = "timing",
			.help = "print operation timings",
			.flags = FPLINUX_CLI_REPEAT,
		},
	};
	struct fplinux_cli cli = {
		.program = argv[0],
		.description =
			"Decode, encode or scale images with the UMS9117 V4L2 devices.\n"
			"Encode requires even raw width and a height. Scale accepts "
			"640x480 or 320x240 NV16 input.",
		.options = cli_options,
		.option_count = sizeof(cli_options) / sizeof(cli_options[0]),
		.parse_option = parse_cli_option,
		.data = options,
	};
	enum fplinux_cli_result result;

	memset(options, 0, sizeof(*options));
	options->operation = FPLINUX_JPEG_DECODE;
	options->scale = 1;
	options->quality = ENCODE_QUALITY;
	options->repeat = 1;
	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	options->input_path = cli_options[CLI_OPTION_INPUT].value;
	options->output_path = cli_options[CLI_OPTION_OUTPUT].value;
	options->device_path = cli_options[CLI_OPTION_DEVICE].value;
	options->timing = cli_options[CLI_OPTION_TIMING].count != 0;
	if (options->operation != FPLINUX_JPEG_ENCODE &&
	    cli_options[CLI_OPTION_QUALITY].count) {
		return fplinux_cli_error(&cli, "only encode accepts --quality");
	}
	if (options->operation == FPLINUX_JPEG_DECODE) {
		if (options->width || options->height) {
			return fplinux_cli_error(
				&cli,
				"decode does not accept --width or --height");
		}
	} else {
		if (options->scale != 1 || !options->width ||
		    !options->height || (options->width & 1U)) {
			return fplinux_cli_error(
				&cli, "encode and scale require even --width, "
				      "--height and --scale 1");
		}
		if (options->operation == FPLINUX_JPEG_SCALE &&
		    !((options->width == SCALE_LARGE_WIDTH &&
		       options->height == SCALE_LARGE_HEIGHT) ||
		      (options->width == SCALE_SMALL_WIDTH &&
		       options->height == SCALE_SMALL_HEIGHT))) {
			return fplinux_cli_error(
				&cli,
				"scale requires 640x480 or 320x240 input");
		}
	}
	return FPLINUX_CLI_READY;
}

static int read_input(const char *path, size_t maximum, size_t expected,
		      uint8_t **data, size_t *length)
{
	struct stat status;
	int fd = -1;
	uint8_t *buffer = NULL;
	size_t offset = 0;

	*data = NULL;
	*length = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		goto fail;
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0 || (uintmax_t)status.st_size > maximum ||
	    (expected && (uintmax_t)status.st_size != expected)) {
		errno = EINVAL;
		goto fail;
	}
	buffer = malloc((size_t)status.st_size);
	if (!buffer)
		goto fail;
	while (offset < (size_t)status.st_size) {
		ssize_t count = read(fd, buffer + offset,
				     (size_t)status.st_size - offset);

		if (count < 0) {
			if (errno == EINTR)
				continue;
			goto fail;
		}
		if (count == 0) {
			errno = EIO;
			goto fail;
		}
		offset += (size_t)count;
	}
	if (close(fd) < 0)
		goto fail_without_close;
	*data = buffer;
	*length = offset;
	return 0;

fail:
	if (fd >= 0)
		(void)close(fd);
fail_without_close:
	free(buffer);
	return -1;
}

static const char *operation_name(enum fplinux_jpeg_operation operation)
{
	switch (operation) {
	case FPLINUX_JPEG_DECODE:
		return "decode";
	case FPLINUX_JPEG_ENCODE:
		return "encode";
	case FPLINUX_JPEG_SCALE:
		return "scale";
	}
	return "unknown";
}

static int write_all(int fd, const uint8_t *data, size_t length)
{
	while (length) {
		ssize_t count = write(fd, data, length);

		if (count < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (count == 0) {
			errno = EIO;
			return -1;
		}
		data += count;
		length -= (size_t)count;
	}
	return 0;
}

static int write_output(const char *path, const uint8_t *data, size_t length)
{
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	if (write_all(fd, data, length) < 0)
		goto fail;
	if (close(fd) < 0)
		return -1;
	return 0;

fail:
	(void)close(fd);
	return -1;
}

static const char *format_name(unsigned int vsub)
{
	return vsub == 2 ? "NV12M" : "NV16M";
}

static uint64_t elapsed_ns(const struct timespec *start,
			   const struct timespec *end)
{
	int64_t seconds = end->tv_sec - start->tv_sec;
	int64_t nanoseconds = end->tv_nsec - start->tv_nsec;

	return (uint64_t)(seconds * 1000000000LL + nanoseconds);
}

static uint64_t elapsed_usage_us(const struct timeval *start,
				 const struct timeval *end)
{
	int64_t seconds = end->tv_sec - start->tv_sec;
	int64_t microseconds = end->tv_usec - start->tv_usec;

	return (uint64_t)(seconds * 1000000LL + microseconds);
}

int main(int argc, char **argv)
{
	struct options options;
	struct timings timings = { 0 };
	uint8_t *input = NULL;
	size_t input_length = 0;
	size_t expected_input_length = 0;
	size_t maximum_input_length = MAX_JPEG_INPUT_BYTES;
	struct fplinux_jpeg_v4l2_request request;
	struct fplinux_jpeg_v4l2_result output = { 0 };
	char error[160];
	int result = EXIT_FAILURE;
	enum fplinux_cli_result parse_result;

	parse_result = parse_options(argc, argv, &options);
	if (parse_result != FPLINUX_CLI_READY)
		return parse_result;
	if (options.timing &&
	    (clock_gettime(CLOCK_MONOTONIC, &timings.started) < 0 ||
	     getrusage(RUSAGE_SELF, &timings.usage_started) < 0)) {
		perror("fplinux-jpeg: timing");
		goto out;
	}
	if (options.operation != FPLINUX_JPEG_DECODE) {
		uint64_t bytes = (uint64_t)options.width * options.height * 2U;

		if (bytes > SIZE_MAX || bytes > MAX_OUTPUT_BYTES) {
			errno = EOVERFLOW;
			perror("fplinux-jpeg: input geometry");
			goto out;
		}
		expected_input_length = (size_t)bytes;
		maximum_input_length = expected_input_length;
	}
	if (read_input(options.input_path, maximum_input_length,
		       expected_input_length, &input, &input_length) < 0) {
		perror("fplinux-jpeg: input");
		goto out;
	}
	if (options.timing &&
	    clock_gettime(CLOCK_MONOTONIC, &timings.input_done) < 0) {
		perror("fplinux-jpeg: timing");
		goto out;
	}
	request = (struct fplinux_jpeg_v4l2_request){
		.device_path = options.device_path,
		.operation = options.operation,
		.scale = options.scale,
		.width = options.width,
		.height = options.height,
		.quality = options.quality,
		.repeat = options.repeat,
		.timing = options.timing,
	};
	if (fplinux_jpeg_v4l2_run(&request, input, input_length, &output,
				  &timings.device, error, sizeof(error)) < 0) {
		fprintf(stderr, "fplinux-jpeg: %s\n", error);
		goto out;
	}
	if (options.timing &&
	    clock_gettime(CLOCK_MONOTONIC, &timings.output_started) < 0) {
		perror("fplinux-jpeg: timing");
		goto out;
	}
	if (write_output(options.output_path, output.data, output.length) < 0) {
		perror("fplinux-jpeg: output");
		goto out;
	}
	if (options.timing &&
	    (clock_gettime(CLOCK_MONOTONIC, &timings.output_done) < 0 ||
	     getrusage(RUSAGE_SELF, &timings.usage_done) < 0)) {
		perror("fplinux-jpeg: timing");
		goto out;
	}
	if (options.operation == FPLINUX_JPEG_DECODE) {
		printf("format=%s width=%u height=%u stride_y=%u stride_uv=%u bytes_y=%u bytes_uv=%u repeat=%u\n",
		       format_name(output.vsub), output.width, output.height,
		       output.stride[0], output.stride[1], output.bytesused[0],
		       output.bytesused[1], options.repeat);
	} else if (options.operation == FPLINUX_JPEG_ENCODE) {
		printf("operation=encode format=JPEG width=%u height=%u bytes=%zu quality=%u repeat=%u\n",
		       options.width, options.height, output.length,
		       output.quality, options.repeat);
	} else {
		printf("operation=scale format=%s width=%u height=%u stride_y=%u stride_uv=%u bytes_y=%u bytes_uv=%u source_width=%u source_height=%u repeat=%u\n",
		       format_name(output.vsub), output.width, output.height,
		       output.stride[0], output.stride[1], output.bytesused[0],
		       output.bytesused[1], options.width, options.height,
		       options.repeat);
	}
	if (options.timing) {
		printf("timing operation=%s repeat=%u wall_input_ns=%llu wall_prepare_ns=%llu wall_loops_ns=%llu wall_warm_loops_ns=%llu wall_queue_ns=%llu wall_warm_queue_ns=%llu wall_wait_ns=%llu wall_warm_wait_ns=%llu wall_output_ns=%llu wall_total_ns=%llu user_total_us=%llu sys_total_us=%llu\n",
		       operation_name(options.operation), options.repeat,
		       (unsigned long long)elapsed_ns(&timings.started,
						      &timings.input_done),
		       (unsigned long long)elapsed_ns(
			       &timings.input_done,
			       &timings.device.prepare_done),
		       (unsigned long long)timings.device.loop_ns,
		       (unsigned long long)timings.device.warm_loop_ns,
		       (unsigned long long)timings.device.queue_ns,
		       (unsigned long long)timings.device.warm_queue_ns,
		       (unsigned long long)timings.device.wait_ns,
		       (unsigned long long)timings.device.warm_wait_ns,
		       (unsigned long long)elapsed_ns(&timings.output_started,
						      &timings.output_done),
		       (unsigned long long)elapsed_ns(&timings.started,
						      &timings.output_done),
		       (unsigned long long)elapsed_usage_us(
			       &timings.usage_started.ru_utime,
			       &timings.usage_done.ru_utime),
		       (unsigned long long)elapsed_usage_us(
			       &timings.usage_started.ru_stime,
			       &timings.usage_done.ru_stime));
		printf("memory_peak_rss_kib=%ld minor_faults=%ld major_faults=%ld\n",
		       timings.usage_done.ru_maxrss,
		       timings.usage_done.ru_minflt,
		       timings.usage_done.ru_majflt);
	}
	result = EXIT_SUCCESS;

out:
	free(output.data);
	free(input);
	return result;
}
