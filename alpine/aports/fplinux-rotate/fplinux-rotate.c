/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-rotate.h"
#include "fplinux-fb-session.h"
#include "fplinux-cli.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
	uint64_t setup_us;
	uint64_t copy_in_us;
	uint64_t queue_us;
	uint64_t wait_us;
	uint64_t copy_out_us;
	uint64_t guard_us;
	uint64_t teardown_us;
	uint64_t cpu_rotate_us;
	uint64_t convert_us;
	uint64_t framebuffer_us;
	uint64_t total_us;
	uint64_t cpu_user_us;
	uint64_t cpu_system_us;
};

struct mapped_queue {
	enum v4l2_buf_type type;
	unsigned int planes;
	uint8_t *data[MAX_PLANES];
	size_t length[MAX_PLANES];
	size_t required[MAX_PLANES];
	uint32_t stride[MAX_PLANES];
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

static int xioctl(int descriptor, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(descriptor, request, argument);
	} while (result < 0 && errno == EINTR);
	return result;
}

static uint32_t format_fourcc(enum fplinux_rotate_format format)
{
	switch (format) {
	case FPLINUX_ROTATE_RGB565:
		return V4L2_PIX_FMT_RGB565;
	case FPLINUX_ROTATE_XRGB32:
		return V4L2_PIX_FMT_XRGB32;
	case FPLINUX_ROTATE_GREY:
		return V4L2_PIX_FMT_GREY;
	case FPLINUX_ROTATE_NV12:
		return V4L2_PIX_FMT_NV12M;
	case FPLINUX_ROTATE_NV16:
		return V4L2_PIX_FMT_NV16M;
	}
	return 0;
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

static bool device_supports(int descriptor, uint32_t fourcc)
{
	struct v4l2_capability capability = { 0 };
	struct v4l2_queryctrl rotate = { .id = V4L2_CID_ROTATE };
	struct v4l2_queryctrl hflip = { .id = V4L2_CID_HFLIP };
	static const enum v4l2_buf_type types[] = {
		V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	};
	unsigned int type;
	uint32_t caps;

	if (xioctl(descriptor, VIDIOC_QUERYCAP, &capability) < 0)
		return false;
	caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS ?
		       capability.device_caps :
		       capability.capabilities;
	if (!(caps & V4L2_CAP_STREAMING) ||
	    !(caps & V4L2_CAP_VIDEO_M2M_MPLANE) ||
	    xioctl(descriptor, VIDIOC_QUERYCTRL, &rotate) < 0 ||
	    xioctl(descriptor, VIDIOC_QUERYCTRL, &hflip) < 0 ||
	    (rotate.flags & V4L2_CTRL_FLAG_DISABLED) ||
	    (hflip.flags & V4L2_CTRL_FLAG_DISABLED))
		return false;
	for (type = 0; type < ARRAY_SIZE(types); ++type) {
		struct v4l2_fmtdesc format = { .type = types[type] };
		bool found = false;

		for (format.index = 0;
		     xioctl(descriptor, VIDIOC_ENUM_FMT, &format) == 0;
		     ++format.index)
			if (format.pixelformat == fourcc) {
				found = true;
				break;
			}
		if (!found)
			return false;
	}
	return true;
}

static int open_rota_device(const char *requested, uint32_t fourcc,
			    char *selected, size_t selected_size)
{
	glob_t matches;
	size_t index;
	int descriptor;

	if (requested) {
		descriptor = open(requested, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (descriptor >= 0 && device_supports(descriptor, fourcc)) {
			snprintf(selected, selected_size, "%s", requested);
			return descriptor;
		}
		if (descriptor >= 0)
			close(descriptor);
		errno = ENODEV;
		return -1;
	}
	memset(&matches, 0, sizeof(matches));
	if (glob("/dev/video*", 0, NULL, &matches) != 0) {
		errno = ENODEV;
		return -1;
	}
	for (index = 0; index < matches.gl_pathc; ++index) {
		descriptor = open(matches.gl_pathv[index],
				  O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (descriptor >= 0 && device_supports(descriptor, fourcc)) {
			snprintf(selected, selected_size, "%s",
				 matches.gl_pathv[index]);
			globfree(&matches);
			return descriptor;
		}
		if (descriptor >= 0)
			close(descriptor);
	}
	globfree(&matches);
	errno = ENODEV;
	return -1;
}

static bool
set_transform_controls(int descriptor,
		       const struct fplinux_rotate_transform *transform)
{
	struct v4l2_ext_control values[] = {
		{ .id = V4L2_CID_ROTATE,
		  .value = (int32_t)transform->rotation },
		{ .id = V4L2_CID_HFLIP, .value = transform->hflip },
	};
	struct v4l2_ext_controls controls = {
		.ctrl_class = V4L2_CTRL_CLASS_USER,
		.count = ARRAY_SIZE(values),
		.controls = values,
	};

	return xioctl(descriptor, VIDIOC_S_EXT_CTRLS, &controls) == 0;
}

static bool set_format(int descriptor, enum v4l2_buf_type type,
		       enum fplinux_rotate_format format, uint32_t width,
		       uint32_t height, uint32_t requested_stride,
		       uint32_t strides[MAX_PLANES], size_t sizes[MAX_PLANES])
{
	struct v4l2_format request = { .type = type };
	unsigned int plane;

	request.fmt.pix_mp.width = width;
	request.fmt.pix_mp.height = height;
	request.fmt.pix_mp.pixelformat = format_fourcc(format);
	request.fmt.pix_mp.field = V4L2_FIELD_NONE;
	request.fmt.pix_mp.num_planes = fplinux_rotate_plane_count(format);
	if (requested_stride)
		for (plane = 0; plane < request.fmt.pix_mp.num_planes; ++plane)
			request.fmt.pix_mp.plane_fmt[plane].bytesperline =
				requested_stride;
	if (xioctl(descriptor, VIDIOC_S_FMT, &request) < 0 ||
	    request.fmt.pix_mp.pixelformat != format_fourcc(format) ||
	    request.fmt.pix_mp.width != width ||
	    request.fmt.pix_mp.height != height ||
	    request.fmt.pix_mp.num_planes !=
		    fplinux_rotate_plane_count(format)) {
		errno = EINVAL;
		return false;
	}
	for (plane = 0; plane < request.fmt.pix_mp.num_planes; ++plane) {
		strides[plane] =
			request.fmt.pix_mp.plane_fmt[plane].bytesperline;
		sizes[plane] = request.fmt.pix_mp.plane_fmt[plane].sizeimage;
		if (strides[plane] <
			    fplinux_rotate_row_bytes(format, plane, width) ||
		    (size_t)strides[plane] >
			    SIZE_MAX / fplinux_rotate_plane_height(
					       format, plane, height) ||
		    sizes[plane] < (size_t)strides[plane] *
					   fplinux_rotate_plane_height(
						   format, plane, height)) {
			errno = EINVAL;
			return false;
		}
	}
	return true;
}

static bool map_queue(int descriptor, enum v4l2_buf_type type,
		      const uint32_t strides[MAX_PLANES],
		      const size_t required[MAX_PLANES],
		      struct mapped_queue *queue)
{
	struct v4l2_requestbuffers request = {
		.count = 1,
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
	};
	struct v4l2_plane planes[MAX_PLANES] = { 0 };
	struct v4l2_buffer buffer = {
		.index = 0,
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
		.length = MAX_PLANES,
		.m.planes = planes,
	};
	unsigned int plane;

	memset(queue, 0, sizeof(*queue));
	queue->type = type;
	if (xioctl(descriptor, VIDIOC_REQBUFS, &request) < 0)
		return false;
	if (request.count == 0U) {
		errno = ENOMEM;
		return false;
	}
	if (xioctl(descriptor, VIDIOC_QUERYBUF, &buffer) < 0)
		return false;
	if (buffer.length == 0U || buffer.length > MAX_PLANES) {
		errno = EINVAL;
		return false;
	}
	queue->planes = buffer.length;
	for (plane = 0; plane < queue->planes; ++plane) {
		if (!planes[plane].length ||
		    planes[plane].length < required[plane]) {
			errno = EINVAL;
			return false;
		}
		queue->length[plane] = planes[plane].length;
		queue->required[plane] = required[plane];
		queue->stride[plane] = strides[plane];
		queue->data[plane] = mmap(NULL, planes[plane].length,
					  PROT_READ | PROT_WRITE, MAP_SHARED,
					  descriptor,
					  planes[plane].m.mem_offset);
		if (queue->data[plane] == MAP_FAILED) {
			queue->data[plane] = NULL;
			return false;
		}
	}
	return true;
}

static void unmap_queue(struct mapped_queue *queue)
{
	unsigned int plane;

	for (plane = 0; plane < queue->planes; ++plane)
		if (queue->data[plane])
			munmap(queue->data[plane], queue->length[plane]);
	memset(queue, 0, sizeof(*queue));
}

static bool qbuf(int descriptor, const struct mapped_queue *queue,
		 const size_t bytesused[MAX_PLANES])
{
	struct v4l2_plane planes[MAX_PLANES] = { 0 };
	struct v4l2_buffer buffer = {
		.index = 0,
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
		.length = queue->planes,
		.m.planes = planes,
	};
	unsigned int plane;

	for (plane = 0; plane < queue->planes; ++plane) {
		planes[plane].length = queue->length[plane];
		planes[plane].bytesused = bytesused ? bytesused[plane] : 0;
	}
	return xioctl(descriptor, VIDIOC_QBUF, &buffer) == 0;
}

static bool dqbuf(int descriptor, const struct mapped_queue *queue)
{
	struct v4l2_plane planes[MAX_PLANES] = { 0 };
	struct v4l2_buffer buffer = {
		.type = queue->type,
		.memory = V4L2_MEMORY_MMAP,
		.length = queue->planes,
		.m.planes = planes,
	};
	unsigned int plane;

	if (xioctl(descriptor, VIDIOC_DQBUF, &buffer) < 0)
		return false;
	if ((buffer.flags & V4L2_BUF_FLAG_ERROR) ||
	    buffer.length != queue->planes) {
		errno = EIO;
		return false;
	}
	for (plane = 0; plane < queue->planes; ++plane)
		if (planes[plane].data_offset != 0U ||
		    planes[plane].bytesused < queue->required[plane] ||
		    planes[plane].bytesused > planes[plane].length) {
			errno = EIO;
			return false;
		}
	return true;
}

static bool wait_for_queues(int descriptor, const struct mapped_queue *output,
			    const struct mapped_queue *capture)
{
	struct pollfd poll_descriptor = {
		.fd = descriptor,
		.events = POLLIN | POLLOUT,
	};
	uint64_t deadline = monotonic_us() + 2000000ULL;
	bool output_done = false;
	bool capture_done = false;

	while (!output_done || !capture_done) {
		uint64_t now = monotonic_us();
		int remaining;
		int result;

		if (now >= deadline) {
			errno = ETIMEDOUT;
			return false;
		}
		remaining = (int)((deadline - now + 999U) / 1000U);
		result = poll(&poll_descriptor, 1, remaining);
		if (result <= 0) {
			if (errno == EINTR)
				continue;
			if (result == 0)
				errno = ETIMEDOUT;
			return false;
		}
		if (!output_done && dqbuf(descriptor, output))
			output_done = true;
		else if (!output_done && errno != EAGAIN)
			return false;
		if (!capture_done && dqbuf(descriptor, capture))
			capture_done = true;
		else if (!capture_done && errno != EAGAIN)
			return false;
	}
	return true;
}

static bool padding_unchanged(const struct mapped_queue *queue,
			      enum fplinux_rotate_format format, uint32_t width,
			      uint32_t height, uint8_t canary)
{
	unsigned int plane;

	for (plane = 0; plane < queue->planes; ++plane) {
		uint32_t rows =
			fplinux_rotate_plane_height(format, plane, height);
		uint32_t active =
			fplinux_rotate_row_bytes(format, plane, width);
		uint32_t row;
		size_t offset;

		for (row = 0; row < rows; ++row)
			for (offset = active; offset < queue->stride[plane];
			     ++offset)
				if (queue->data[plane]
					       [(size_t)row *
							queue->stride[plane] +
						offset] != canary)
					return false;
		for (offset = (size_t)queue->stride[plane] * rows;
		     offset < queue->length[plane]; ++offset)
			if (queue->data[plane][offset] != canary)
				return false;
	}
	return true;
}

static bool run_rota(const struct options *options,
		     const struct fplinux_rotate_image *source,
		     struct fplinux_rotate_image *destination,
		     struct timing *timing)
{
	struct mapped_queue output = { 0 };
	struct mapped_queue capture = { 0 };
	uint32_t output_strides[MAX_PLANES] = { 0 };
	uint32_t capture_strides[MAX_PLANES] = { 0 };
	size_t output_sizes[MAX_PLANES] = { 0 };
	size_t capture_sizes[MAX_PLANES] = { 0 };
	size_t bytesused[MAX_PLANES] = { 0 };
	struct v4l2_selection selection = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.target = V4L2_SEL_TGT_CROP,
	};
	uint32_t destination_width;
	uint32_t destination_height;
	char device[128];
	uint64_t started = monotonic_us();
	uint64_t stage_started;
	uint64_t wait_started;
	unsigned int plane;
	int descriptor = -1;
	bool output_streaming = false;
	bool capture_streaming = false;
	bool ok = false;

	if (!fplinux_rotate_dimensions(&options->transform, &destination_width,
				       &destination_height))
		return false;
	descriptor = open_rota_device(options->device,
				      format_fourcc(options->format), device,
				      sizeof(device));
	if (descriptor < 0 ||
	    !set_format(descriptor, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			options->format, source->width, source->height,
			source->plane[0].stride, output_strides, output_sizes))
		goto out;
	selection.r.left = options->transform.left;
	selection.r.top = options->transform.top;
	selection.r.width = options->transform.width;
	selection.r.height = options->transform.height;
	if (xioctl(descriptor, VIDIOC_S_SELECTION, &selection) < 0 ||
	    selection.r.left != (int32_t)options->transform.left ||
	    selection.r.top != (int32_t)options->transform.top ||
	    selection.r.width != options->transform.width ||
	    selection.r.height != options->transform.height ||
	    !set_transform_controls(descriptor, &options->transform) ||
	    !set_format(descriptor, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			options->format, destination_width, destination_height,
			0, capture_strides, capture_sizes) ||
	    !map_queue(descriptor, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		       output_strides, output_sizes, &output) ||
	    !map_queue(descriptor, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		       capture_strides, capture_sizes, &capture) ||
	    output.planes != source->planes || capture.planes != source->planes)
		goto out;
	timing->setup_us += monotonic_us() - started;
	stage_started = monotonic_us();
	for (plane = 0; plane < source->planes; ++plane) {
		uint32_t rows = fplinux_rotate_plane_height(
			source->format, plane, source->height);
		uint32_t row_bytes = fplinux_rotate_row_bytes(
			source->format, plane, source->width);
		uint32_t row;

		if (output.length[plane] < output_sizes[plane] ||
		    capture.length[plane] < capture_sizes[plane])
			goto out;
		if (options->verify)
			memset(output.data[plane], 0x6d, output.length[plane]);
		for (row = 0; row < rows; ++row)
			memcpy(output.data[plane] +
				       (size_t)row * output.stride[plane],
			       source->plane[plane].data +
				       (size_t)row *
					       source->plane[plane].stride,
			       row_bytes);
		bytesused[plane] = output_sizes[plane];
		if (options->verify)
			memset(capture.data[plane], CAPTURE_CANARY,
			       capture.length[plane]);
	}
	timing->copy_in_us += monotonic_us() - stage_started;
	stage_started = monotonic_us();
	if (!qbuf(descriptor, &output, bytesused) ||
	    !qbuf(descriptor, &capture, NULL) ||
	    xioctl(descriptor, VIDIOC_STREAMON, &capture.type) < 0)
		goto out;
	capture_streaming = true;
	if (xioctl(descriptor, VIDIOC_STREAMON, &output.type) < 0)
		goto out;
	output_streaming = true;
	timing->queue_us += monotonic_us() - stage_started;
	wait_started = monotonic_us();
	if (!wait_for_queues(descriptor, &output, &capture))
		goto out;
	timing->wait_us += monotonic_us() - wait_started;
	stage_started = monotonic_us();
	for (plane = 0; plane < source->planes; ++plane) {
		uint32_t destination_rows = fplinux_rotate_plane_height(
			options->format, plane, destination_height);
		uint32_t destination_bytes = fplinux_rotate_row_bytes(
			options->format, plane, destination_width);
		uint32_t row;

		for (row = 0; row < destination_rows; ++row)
			memcpy(destination->plane[plane].data +
				       (size_t)row *
					       destination->plane[plane].stride,
			       capture.data[plane] +
				       (size_t)row * capture.stride[plane],
			       destination_bytes);
	}
	timing->copy_out_us += monotonic_us() - stage_started;
	stage_started = monotonic_us();
	if (options->verify) {
		for (plane = 0; plane < source->planes; ++plane) {
			uint32_t rows = fplinux_rotate_plane_height(
				source->format, plane, source->height);
			uint32_t bytes = fplinux_rotate_row_bytes(
				source->format, plane, source->width);
			uint32_t row;

			for (row = 0; row < rows; ++row)
				if (memcmp(output.data[plane] +
						   (size_t)row *
							   output.stride[plane],
					   source->plane[plane].data +
						   (size_t)row *
							   source->plane[plane]
								   .stride,
					   bytes) != 0)
					goto out;
		}
		if (!padding_unchanged(&output, options->format, source->width,
				       source->height, 0x6dU) ||
		    !padding_unchanged(&capture, options->format,
				       destination_width, destination_height,
				       CAPTURE_CANARY)) {
			errno = EIO;
			goto out;
		}
		timing->guard_us += monotonic_us() - stage_started;
	}
	ok = true;
out:
	started = monotonic_us();
	if (output_streaming)
		xioctl(descriptor, VIDIOC_STREAMOFF, &output.type);
	if (capture_streaming)
		xioctl(descriptor, VIDIOC_STREAMOFF, &capture.type);
	unmap_queue(&capture);
	unmap_queue(&output);
	if (descriptor >= 0)
		close(descriptor);
	timing->teardown_us += monotonic_us() - started;
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
	struct fplinux_fb_session session;
	char error[160];
	uint64_t full_started = monotonic_us();
	uint64_t convert_started;
	unsigned int page;
	uint16_t *pixels;
	bool ok;

	if (!fplinux_fb_session_open(&session, "/dev/fb0", "/dev/tty0", error,
				     sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		return false;
	}
	if (session.width != image->width || session.height != image->height ||
	    !fplinux_fb_session_set_graphics(&session, error, sizeof(error))) {
		if (session.width != image->width ||
		    session.height != image->height)
			fprintf(stderr,
				"preview is %ux%u, framebuffer is %ux%u\n",
				image->width, image->height, session.width,
				session.height);
		else
			fprintf(stderr, "%s\n", error);
		fplinux_fb_session_close(&session);
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
		ok = fplinux_fb_session_present(&session, page);
	}
	if (ok && display_ms) {
		uint64_t hold_started = monotonic_us();

		usleep(display_ms * 1000U);
		full_started += monotonic_us() - hold_started;
	}
	ok = fplinux_fb_session_close(&session) && ok;
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
	uint64_t guards_before = timing->guard_us;
	bool ok;

	getrusage(RUSAGE_SELF, &before);
	if (options->engine == ENGINE_CPU) {
		uint64_t stage = monotonic_us();

		ok = fplinux_rotate_cpu(source, destination,
					&options->transform);
		timing->cpu_rotate_us += monotonic_us() - stage;
	} else
		ok = run_rota(options, source, destination, timing);
	if (ok && options->display)
		ok = present_image(
			destination,
			options->benchmark ? 0U : options->display_ms, timing);
	getrusage(RUSAGE_SELF, &after);
	timing->cpu_user_us +=
		timeval_us(after.ru_utime) - timeval_us(before.ru_utime);
	timing->cpu_system_us +=
		timeval_us(after.ru_stime) - timeval_us(before.ru_stime);
	timing->total_us +=
		monotonic_us() - started - (timing->guard_us - guards_before);
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
	       (unsigned long long)timing->setup_us,
	       (unsigned long long)timing->copy_in_us,
	       (unsigned long long)timing->queue_us,
	       (unsigned long long)timing->wait_us,
	       (unsigned long long)timing->copy_out_us,
	       (unsigned long long)timing->guard_us,
	       (unsigned long long)timing->teardown_us,
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
		fprintf(stderr, "cannot allocate images\n");
		goto out;
	}
	if (!read_image(options.input, &source) ||
	    !fplinux_rotate_validate(&source, &destination,
				     &options.transform)) {
		fprintf(stderr,
			"invalid image, crop, stride, or unsupported transform: %s\n",
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
						"benchmark %s failed at iteration %u: %s\n",
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
					"%s rotation or verification failed at iteration %u: %s\n",
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
		fprintf(stderr, "cannot write %s: %s\n", options.output,
			strerror(errno));
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
