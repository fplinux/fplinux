/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE

#include "rotate-rota.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define MAX_PLANES 2U
#define CAPTURE_CANARY 0xa5U

struct mapped_queue {
	enum v4l2_buf_type type;
	unsigned int planes;
	uint8_t *data[MAX_PLANES];
	size_t length[MAX_PLANES];
	size_t required[MAX_PLANES];
	uint32_t stride[MAX_PLANES];
};

static uint64_t monotonic_us(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000000ULL + value.tv_nsec / 1000U;
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

bool fplinux_rotate_rota_run(const char *requested_device,
			     const struct fplinux_rotate_transform *transform,
			     bool verify,
			     const struct fplinux_rotate_image *source,
			     struct fplinux_rotate_image *destination,
			     struct fplinux_rotate_rota_timing *timing)
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

	if (!fplinux_rotate_dimensions(transform, &destination_width,
				       &destination_height))
		return false;
	descriptor = open_rota_device(requested_device,
				      format_fourcc(source->format), device,
				      sizeof(device));
	if (descriptor < 0 ||
	    !set_format(descriptor, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			source->format, source->width, source->height,
			source->plane[0].stride, output_strides, output_sizes))
		goto out;
	selection.r.left = transform->left;
	selection.r.top = transform->top;
	selection.r.width = transform->width;
	selection.r.height = transform->height;
	if (xioctl(descriptor, VIDIOC_S_SELECTION, &selection) < 0 ||
	    selection.r.left != (int32_t)transform->left ||
	    selection.r.top != (int32_t)transform->top ||
	    selection.r.width != transform->width ||
	    selection.r.height != transform->height ||
	    !set_transform_controls(descriptor, transform) ||
	    !set_format(descriptor, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			source->format, destination_width, destination_height,
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
		if (verify)
			memset(output.data[plane], 0x6d, output.length[plane]);
		for (row = 0; row < rows; ++row)
			memcpy(output.data[plane] +
				       (size_t)row * output.stride[plane],
			       source->plane[plane].data +
				       (size_t)row *
					       source->plane[plane].stride,
			       row_bytes);
		bytesused[plane] = output_sizes[plane];
		if (verify)
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
			source->format, plane, destination_height);
		uint32_t destination_bytes = fplinux_rotate_row_bytes(
			source->format, plane, destination_width);
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
	if (verify) {
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
		if (!padding_unchanged(&output, source->format, source->width,
				       source->height, 0x6dU) ||
		    !padding_unchanged(&capture, source->format,
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
