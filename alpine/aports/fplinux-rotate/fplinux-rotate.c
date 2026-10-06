/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-rotate.h"
#include "rotate-rota.h"
#include "fplinux-drm-session.h"
#include "fplinux-cli.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define MAX_PLANES 2U
#define CAPTURE_CANARY 0xa5U

enum engine { ENGINE_CPU, ENGINE_ROTA };

struct options {
	enum engine engine;
	enum fplinux_rotate_format format;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	struct fplinux_rotate_transform transform;
	const char *input;
	const char *output;
	const char *device;
	bool display;
	uint32_t display_ms;
	bool verify;
	unsigned int benchmark;
	unsigned int iterations;
};

struct timing {
	struct fplinux_rotate_rota_timing rota;
	uint64_t cpu_rotate_us;
	uint64_t convert_us;
	uint64_t framebuffer_us;
	uint64_t total_us;
	uint64_t cpu_user_us;
	uint64_t cpu_system_us;
};

static const char *const format_names[] = {
	"rgb565", "xrgb32", "grey", "nv12", "nv16",
};

static uint64_t monotonic_us(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000000ULL + value.tv_nsec / 1000U;
}

static uint64_t timeval_us(struct timeval value)
{
	return (uint64_t)value.tv_sec * 1000000ULL + value.tv_usec;
}

static const char *format_name(enum fplinux_rotate_format format)
{
	return format_names[format];
}

static bool parse_crop_component(const char **text, char terminator,
				 uint32_t *value)
{
	const unsigned char *cursor = (const unsigned char *)*text;
	char *end;
	unsigned long parsed;

	while (isspace(*cursor))
		++cursor;
	if (*cursor == '-')
		return false;
	errno = 0;
	parsed = strtoul(*text, &end, 10);
	if (errno || end == *text || parsed > UINT32_MAX || *end != terminator)
		return false;
	*value = (uint32_t)parsed;
	*text = terminator ? end + 1 : end;
	return true;
}

static bool parse_crop(const char *text, struct fplinux_rotate_transform *value)
{
	struct fplinux_rotate_transform parsed = *value;

	if (!parse_crop_component(&text, ',', &parsed.left) ||
	    !parse_crop_component(&text, ',', &parsed.top) ||
	    !parse_crop_component(&text, ',', &parsed.width) ||
	    !parse_crop_component(&text, '\0', &parsed.height))
		return false;
	*value = parsed;
	return true;
}

enum option_index {
	OPT_ENGINE,
	OPT_FORMAT,
	OPT_WIDTH,
	OPT_HEIGHT,
	OPT_STRIDE,
	OPT_CROP,
	OPT_ROTATION,
	OPT_HFLIP,
	OPT_VFLIP,
	OPT_INPUT,
	OPT_OUTPUT,
	OPT_DEVICE,
	OPT_DISPLAY,
	OPT_DISPLAY_MS,
	OPT_VERIFY,
	OPT_ITERATIONS,
	OPT_BENCHMARK,
};

static const char *parse_option(size_t option, const char *value, void *data)
{
	struct options *options = data;
	unsigned int *number;
	unsigned int minimum = 0;
	unsigned int maximum = UINT32_MAX;
	unsigned int candidate;

	switch (option) {
	case OPT_ENGINE:
		if (!strcmp(value, "cpu"))
			options->engine = ENGINE_CPU;
		else if (!strcmp(value, "rota"))
			options->engine = ENGINE_ROTA;
		else
			goto invalid;
		return NULL;
	case OPT_FORMAT:
		for (candidate = 0; candidate < ARRAY_SIZE(format_names);
		     ++candidate) {
			if (!strcmp(value, format_names[candidate])) {
				options->format =
					(enum fplinux_rotate_format)candidate;
				return NULL;
			}
		}
		goto invalid;
	case OPT_WIDTH:
		number = &options->width;
		break;
	case OPT_HEIGHT:
		number = &options->height;
		break;
	case OPT_STRIDE:
		number = &options->stride;
		break;
	case OPT_CROP:
		if (!parse_crop(value, &options->transform))
			goto invalid;
		return NULL;
	case OPT_ROTATION:
		number = &options->transform.rotation;
		break;
	case OPT_DISPLAY:
		options->display = true;
		options->display_ms = 2000U;
		return NULL;
	case OPT_DISPLAY_MS:
		options->display = true;
		number = &options->display_ms;
		maximum = 60000;
		break;
	case OPT_ITERATIONS:
		number = &options->iterations;
		minimum = 1;
		maximum = 10000;
		break;
	case OPT_BENCHMARK:
		number = &options->benchmark;
		minimum = 1;
		maximum = 10000;
		break;
	default:
		return NULL;
	}
	if (fplinux_cli_unsigned(value, minimum, maximum, number))
		return NULL;
invalid:
	return "invalid option value or combination";
}

static bool options_valid(struct options *options)
{
	if (options->width == 0U || options->height == 0U)
		return false;
	if (options->transform.width == 0U) {
		options->transform.width = options->width;
		options->transform.height = options->height;
	}
	if (options->stride == 0U)
		options->stride = fplinux_rotate_row_bytes(options->format, 0,
							   options->width);
	return fplinux_rotate_row_bytes(options->format, 0, options->width) !=
		       0U &&
	       ((options->transform.rotation == 0U &&
		 options->transform.hflip && !options->transform.vflip) ||
		((options->transform.rotation == 90U ||
		  options->transform.rotation == 180U ||
		  options->transform.rotation == 270U) &&
		 !options->transform.hflip && !options->transform.vflip)) &&
	       options->stride >= fplinux_rotate_row_bytes(options->format, 0,
							   options->width) &&
	       (!options->output || !strncmp(options->output, "/run/", 5) ||
		!strncmp(options->output, "/tmp/", 5));
}

static int parse_options(int argc, char **argv, struct options *options)
{
	struct fplinux_cli_option arguments[] = {
		[OPT_ENGINE] = {
			.name = "engine",
			.metavar = "cpu|rota",
			.help = "Rotation engine",
			.flags = FPLINUX_CLI_REQUIRED | FPLINUX_CLI_REPEAT,
		},
		[OPT_FORMAT] = {
			.name = "format",
			.metavar = "FORMAT",
			.help = "Pixel format: rgb565, xrgb32, grey, nv12, nv16",
			.flags = FPLINUX_CLI_REQUIRED | FPLINUX_CLI_REPEAT,
		},
		[OPT_WIDTH] = {
			.name = "width",
			.metavar = "N",
			.help = "Source width in pixels",
			.flags = FPLINUX_CLI_REQUIRED | FPLINUX_CLI_REPEAT,
		},
		[OPT_HEIGHT] = {
			.name = "height",
			.metavar = "N",
			.help = "Source height in pixels",
			.flags = FPLINUX_CLI_REQUIRED | FPLINUX_CLI_REPEAT,
		},
		[OPT_STRIDE] = {
			.name = "stride",
			.metavar = "N",
			.help = "Source stride in bytes (default: packed row)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_CROP] = {
			.name = "crop",
			.metavar = "X,Y,W,H",
			.help = "Source crop (default: whole image)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_ROTATION] = {
			.name = "rotate",
			.metavar = "0|90|180|270",
			.help = "Rotation in degrees (default: 90)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_HFLIP] = {
			.name = "hflip",
			.help = "Flip horizontally (requires --rotate 0)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_VFLIP] = {
			.name = "vflip",
			.help = "Flip vertically (currently unsupported)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_INPUT] = {
			.name = "input",
			.metavar = "PATH",
			.help = "Raw planes in order (default: deterministic corpus)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_OUTPUT] = {
			.name = "output",
			.metavar = "PATH",
			.help = "Output below /run or /tmp (default: /run/fplinux-rotate.raw)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_DEVICE] = {
			.name = "device",
			.metavar = "PATH",
			.help = "Matching V4L2 mem2mem device (default: discover)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_DISPLAY] = {
			.name = "display",
			.help = "Present a native-size RGB565 preview for 2000 ms",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_DISPLAY_MS] = {
			.name = "display-ms",
			.metavar = "N",
			.help = "Present a preview for 0..60000 ms; last display option wins",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_VERIFY] = {
			.name = "verify",
			.help = "Compare output with the CPU reference",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_ITERATIONS] = {
			.name = "iterations",
			.metavar = "N",
			.help = "Repeat the selected engine 1..10000 times (default: 1)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[OPT_BENCHMARK] = {
			.name = "benchmark",
			.metavar = "N",
			.help = "Run CPU/ROTA/ROTA/CPU batches of 1..10000 iterations",
			.flags = FPLINUX_CLI_REPEAT,
		},
	};
	struct fplinux_cli cli = {
		.program = "fplinux-rotate",
		.description = "Rotate, verify and preview raw images.",
		.options = arguments,
		.option_count = ARRAY_SIZE(arguments),
		.parse_option = parse_option,
		.data = options,
	};
	int status;

	memset(options, 0, sizeof(*options));
	options->output = "/run/fplinux-rotate.raw";
	options->transform.rotation = 90U;
	options->iterations = 1;
	status = fplinux_cli_parse(&cli, argc, argv);
	if (status != FPLINUX_CLI_READY)
		return status;
	options->transform.hflip = arguments[OPT_HFLIP].count != 0;
	options->transform.vflip = arguments[OPT_VFLIP].count != 0;
	options->verify = arguments[OPT_VERIFY].count != 0;
	options->input = arguments[OPT_INPUT].value;
	if (arguments[OPT_OUTPUT].count)
		options->output = arguments[OPT_OUTPUT].value;
	options->device = arguments[OPT_DEVICE].value;
	if (!options_valid(options))
		return fplinux_cli_error(&cli,
					 "invalid option value or combination");
	return FPLINUX_CLI_READY;
}

static bool allocate_image(struct fplinux_rotate_image *image,
			   enum fplinux_rotate_format format, uint32_t width,
			   uint32_t height, const uint32_t strides[MAX_PLANES])
{
	unsigned int plane;

	memset(image, 0, sizeof(*image));
	image->format = format;
	image->width = width;
	image->height = height;
	image->planes = fplinux_rotate_plane_count(format);
	for (plane = 0; plane < image->planes; ++plane) {
		uint32_t rows =
			fplinux_rotate_plane_height(format, plane, height);
		uint32_t row_bytes =
			fplinux_rotate_row_bytes(format, plane, width);

		image->plane[plane].stride =
			strides && strides[plane] ? strides[plane] : row_bytes;
		if (image->plane[plane].stride < row_bytes ||
		    (size_t)image->plane[plane].stride > SIZE_MAX / rows)
			goto fail;
		image->plane[plane].size =
			(size_t)image->plane[plane].stride * rows;
		image->plane[plane].data = malloc(image->plane[plane].size);
		if (!image->plane[plane].data)
			goto fail;
	}
	return true;
fail:
	while (plane)
		free(image->plane[--plane].data);
	memset(image, 0, sizeof(*image));
	return false;
}

static void free_image(struct fplinux_rotate_image *image)
{
	unsigned int plane;

	for (plane = 0; plane < image->planes; ++plane)
		free(image->plane[plane].data);
	memset(image, 0, sizeof(*image));
}

static bool read_image(const char *path, struct fplinux_rotate_image *image)
{
	FILE *stream;
	unsigned int plane;

	if (!path) {
		fplinux_rotate_fill_corpus(image, 0x31U);
		return true;
	}
	stream = fopen(path, "rb");
	if (!stream)
		return false;
	for (plane = 0; plane < image->planes; ++plane)
		if (fread(image->plane[plane].data, 1, image->plane[plane].size,
			  stream) != image->plane[plane].size)
			break;
	if (plane != image->planes || fgetc(stream) != EOF) {
		errno = EINVAL;
		fclose(stream);
		return false;
	}
	return fclose(stream) == 0;
}

static bool write_image(const char *path,
			const struct fplinux_rotate_image *image)
{
	FILE *stream;
	unsigned int plane;
	bool ok;

	if (!path)
		return true;
	stream = fopen(path, "wb");
	if (!stream)
		return false;
	for (plane = 0; plane < image->planes; ++plane)
		if (fwrite(image->plane[plane].data, 1,
			   image->plane[plane].size,
			   stream) != image->plane[plane].size)
			break;
	ok = plane == image->planes;
	if (fclose(stream) != 0)
		ok = false;
	return ok;
}

static bool compare_active(const struct fplinux_rotate_image *left,
			   const struct fplinux_rotate_image *right)
{
	unsigned int plane;

	if (left->format != right->format || left->width != right->width ||
	    left->height != right->height || left->planes != right->planes) {
		errno = EILSEQ;
		return false;
	}
	for (plane = 0; plane < left->planes; ++plane) {
		uint32_t rows = fplinux_rotate_plane_height(left->format, plane,
							    left->height);
		uint32_t bytes = fplinux_rotate_row_bytes(left->format, plane,
							  left->width);
		uint32_t row;

		for (row = 0; row < rows; ++row) {
			if (memcmp(left->plane[plane].data +
					   (size_t)row *
						   left->plane[plane].stride,
				   right->plane[plane].data +
					   (size_t)row *
						   right->plane[plane].stride,
				   bytes) != 0) {
				errno = EILSEQ;
				return false;
			}
		}
	}
	return true;
}

static bool present_image(const struct fplinux_rotate_image *image,
			  uint32_t display_ms, struct timing *timing)
{
	struct fplinux_drm_session session;
	char error[160];
	uint64_t full_started = monotonic_us();
	uint64_t convert_started;
	unsigned int page;
	uint16_t *pixels;
	bool ok;

	if (!fplinux_drm_session_open(&session, "/dev/dri/card0", NULL,
				      DRM_FORMAT_RGB565, error,
				      sizeof(error))) {
		fprintf(stderr, "fplinux-rotate: %s\n", error);
		return false;
	}
	if (session.width != image->width || session.height != image->height) {
		fprintf(stderr,
			"fplinux-rotate: preview is %ux%u, framebuffer is %ux%u\n",
			image->width, image->height, session.width,
			session.height);
		fplinux_drm_session_close(&session);
		return false;
	}
	page = session.pages == 2U ? 1U - session.shown_page : 0U;
	pixels = (uint16_t *)(session.mapping +
			      (size_t)page * session.page_bytes);
	convert_started = monotonic_us();
	ok = fplinux_rotate_to_rgb565(image, pixels, session.stride / 2U);
	timing->convert_us += monotonic_us() - convert_started;
	if (ok) {
		__sync_synchronize();
		ok = fplinux_drm_session_present(&session, page);
	}
	if (ok && display_ms) {
		uint64_t hold_started = monotonic_us();

		struct timespec deadline;
		uint64_t deadline_us =
			hold_started + (uint64_t)display_ms * 1000U;

		deadline.tv_sec = deadline_us / 1000000U;
		deadline.tv_nsec = (deadline_us % 1000000U) * 1000U;
		ok = fplinux_drm_session_wait_until(&session, &deadline);
		full_started += monotonic_us() - hold_started;
	}
	ok = fplinux_drm_session_close(&session) && ok;
	timing->framebuffer_us += monotonic_us() - full_started;
	return ok;
}

static bool execute(const struct options *options,
		    const struct fplinux_rotate_image *source,
		    struct fplinux_rotate_image *destination,
		    struct timing *timing)
{
	struct rusage before;
	struct rusage after;
	uint64_t started = monotonic_us();
	uint64_t guards_before = timing->rota.guard_us;
	bool ok;

	getrusage(RUSAGE_SELF, &before);
	if (options->engine == ENGINE_CPU) {
		uint64_t stage = monotonic_us();

		ok = fplinux_rotate_cpu(source, destination,
					&options->transform);
		timing->cpu_rotate_us += monotonic_us() - stage;
	} else
		ok = fplinux_rotate_rota_run(options->device,
					     &options->transform,
					     options->verify, source,
					     destination, &timing->rota);
	if (ok && options->display)
		ok = present_image(
			destination,
			options->benchmark ? 0U : options->display_ms, timing);
	getrusage(RUSAGE_SELF, &after);
	timing->cpu_user_us +=
		timeval_us(after.ru_utime) - timeval_us(before.ru_utime);
	timing->cpu_system_us +=
		timeval_us(after.ru_stime) - timeval_us(before.ru_stime);
	timing->total_us += monotonic_us() - started -
			    (timing->rota.guard_us - guards_before);
	return ok;
}

static void print_timing(const char *stage, enum engine engine,
			 unsigned int iterations, const struct timing *timing)
{
	printf("benchmark stage=%s engine=%s iterations=%u setup_us=%llu copy_in_us=%llu "
	       "dma_queue_us=%llu wait_us=%llu copy_out_us=%llu guard_us=%llu teardown_us=%llu "
	       "cpu_rotate_us=%llu preview_convert_us=%llu full_framebuffer_us=%llu "
	       "total_us=%llu process_user_us=%llu process_system_us=%llu\n",
	       stage, engine == ENGINE_CPU ? "cpu" : "rota", iterations,
	       (unsigned long long)timing->rota.setup_us,
	       (unsigned long long)timing->rota.copy_in_us,
	       (unsigned long long)timing->rota.queue_us,
	       (unsigned long long)timing->rota.wait_us,
	       (unsigned long long)timing->rota.copy_out_us,
	       (unsigned long long)timing->rota.guard_us,
	       (unsigned long long)timing->rota.teardown_us,
	       (unsigned long long)timing->cpu_rotate_us,
	       (unsigned long long)timing->convert_us,
	       (unsigned long long)timing->framebuffer_us,
	       (unsigned long long)timing->total_us,
	       (unsigned long long)timing->cpu_user_us,
	       (unsigned long long)timing->cpu_system_us);
}

int main(int argc, char **argv)
{
	static const enum engine order[] = {
		ENGINE_CPU,
		ENGINE_ROTA,
		ENGINE_ROTA,
		ENGINE_CPU,
	};
	static const char *const stages[] = { "A1", "B1", "B2", "A2" };
	struct fplinux_rotate_image source = { 0 };
	struct fplinux_rotate_image destination = { 0 };
	struct fplinux_rotate_image reference = { 0 };
	struct options options;
	uint32_t destination_width;
	uint32_t destination_height;
	uint32_t source_strides[MAX_PLANES] = { 0 };
	unsigned int sequence;
	bool ok = false;
	int parse_status = parse_options(argc, argv, &options);

	if (parse_status != FPLINUX_CLI_READY)
		return parse_status;
	if (!fplinux_rotate_dimensions(&options.transform, &destination_width,
				       &destination_height)) {
		return fplinux_cli_error(
			&(const struct fplinux_cli){ .program =
							     "fplinux-rotate" },
			"invalid crop dimensions");
	}
	source_strides[0] = options.stride;
	if (fplinux_rotate_plane_count(options.format) == 2U)
		source_strides[1] = options.stride;
	if (!allocate_image(&source, options.format, options.width,
			    options.height, source_strides) ||
	    !allocate_image(&destination, options.format, destination_width,
			    destination_height, NULL) ||
	    (options.verify &&
	     !allocate_image(&reference, options.format, destination_width,
			     destination_height, NULL))) {
		fprintf(stderr, "fplinux-rotate: cannot allocate images\n");
		goto out;
	}
	if (!read_image(options.input, &source) ||
	    !fplinux_rotate_validate(&source, &destination,
				     &options.transform)) {
		fprintf(stderr,
			"fplinux-rotate: invalid image, crop, stride, or unsupported transform: %s\n",
			strerror(errno));
		goto out;
	}
	if (options.verify &&
	    !fplinux_rotate_cpu(&source, &reference, &options.transform))
		goto out;
	if (options.benchmark) {
		for (sequence = 0; sequence < ARRAY_SIZE(order); ++sequence) {
			struct timing timing = { 0 };
			unsigned int iteration;

			options.engine = order[sequence];
			for (iteration = 0; iteration < options.benchmark;
			     ++iteration) {
				memset(destination.plane[0].data,
				       CAPTURE_CANARY,
				       destination.plane[0].size);
				if (destination.planes == 2U)
					memset(destination.plane[1].data,
					       CAPTURE_CANARY,
					       destination.plane[1].size);
				if (!execute(&options, &source, &destination,
					     &timing) ||
				    (options.verify &&
				     !compare_active(&destination,
						     &reference))) {
					fprintf(stderr,
						"fplinux-rotate: benchmark %s failed at iteration %u: %s\n",
						stages[sequence], iteration,
						strerror(errno));
					goto out;
				}
			}
			print_timing(stages[sequence], order[sequence],
				     options.benchmark, &timing);
		}
	} else {
		struct timing timing = { 0 };
		unsigned int iteration;

		for (iteration = 0; iteration < options.iterations;
		     ++iteration) {
			if (!execute(&options, &source, &destination,
				     &timing) ||
			    (options.verify &&
			     !compare_active(&destination, &reference))) {
				fprintf(stderr,
					"fplinux-rotate: %s rotation or verification failed at iteration %u: %s\n",
					options.engine == ENGINE_CPU ? "CPU" :
								       "ROTA",
					iteration, strerror(errno));
				goto out;
			}
		}
		print_timing(options.iterations == 1U ? "single" : "selected",
			     options.engine, options.iterations, &timing);
	}
	if (!write_image(options.output, &destination)) {
		fprintf(stderr, "fplinux-rotate: cannot write %s: %s\n",
			options.output, strerror(errno));
		goto out;
	}
	printf("result format=%s width=%u height=%u path=%s verified=%s\n",
	       format_name(options.format), destination.width,
	       destination.height, options.output,
	       options.verify ? "yes" : "no");
	ok = true;
out:
	free_image(&reference);
	free_image(&destination);
	free_image(&source);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
