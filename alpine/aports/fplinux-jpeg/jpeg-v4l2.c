/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE

#include "jpeg-v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DRIVER_NAME "ums9117-jpeg"
#define DECODER_CARD "UMS9117 JPEG decoder"
#define ENCODER_CARD "UMS9117 JPEG encoder"
#define SCALER_CARD "UMS9117 RAM scaler"
#define MAX_JPEG_INPUT_BYTES (1024U * 1024U)
#define MAX_OUTPUT_BYTES (64U * 1024U * 1024U)
#define MIN_OUTPUT_BYTES 4096U
#define TIMEOUT_MS 10000
#define MAX_PLANES VIDEO_MAX_PLANES

static void set_error(char *error, size_t error_size, const char *stage)
{
	snprintf(error, error_size, "%s: %s", stage, strerror(errno));
}

struct mapped_buffer {
	void *addr[MAX_PLANES];
	size_t length[MAX_PLANES];
	unsigned int nplanes;
};

struct capture_layout {
	uint32_t fourcc;
	uint32_t width;
	uint32_t height;
	uint32_t stride[2];
	uint32_t sizeimage[2];
	struct v4l2_rect crop;
	unsigned int vsub;
	unsigned int decode_factor;
};

struct queue_format {
	uint32_t fourcc;
	uint32_t width;
	uint32_t height;
	uint32_t stride[MAX_PLANES];
	uint32_t sizeimage[MAX_PLANES];
	unsigned int nplanes;
};

static int xioctl(int fd, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && errno == EINTR);
	return result;
}

static const char *operation_card(enum fplinux_jpeg_operation operation)
{
	switch (operation) {
	case FPLINUX_JPEG_DECODE:
		return DECODER_CARD;
	case FPLINUX_JPEG_ENCODE:
		return ENCODER_CARD;
	case FPLINUX_JPEG_SCALE:
		return SCALER_CARD;
	}
	return "";
}

static const char *operation_driver(enum fplinux_jpeg_operation operation)
{
	(void)operation;
	return DRIVER_NAME;
}

static bool is_expected_device(int fd, enum fplinux_jpeg_operation operation)
{
	struct v4l2_capability capability;
	uint32_t capabilities;

	memset(&capability, 0, sizeof(capability));
	if (xioctl(fd, VIDIOC_QUERYCAP, &capability) < 0)
		return false;
	capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ?
			       capability.device_caps :
			       capability.capabilities;
	return !strcmp((const char *)capability.driver,
		       operation_driver(operation)) &&
	       !strcmp((const char *)capability.card,
		       operation_card(operation)) &&
	       (capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) &&
	       (capabilities & V4L2_CAP_STREAMING);
}

static int open_device(const char *requested_path,
		       enum fplinux_jpeg_operation operation)
{
	glob_t matches;
	int fd = -1;
	size_t i;

	if (requested_path) {
		fd = open(requested_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			return -1;
		if (!is_expected_device(fd, operation)) {
			errno = ENODEV;
			(void)close(fd);
			return -1;
		}
		return fd;
	}
	memset(&matches, 0, sizeof(matches));
	if (glob("/dev/video*", 0, NULL, &matches) != 0) {
		errno = ENODEV;
		return -1;
	}
	for (i = 0; i < matches.gl_pathc; ++i) {
		fd = open(matches.gl_pathv[i], O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (is_expected_device(fd, operation))
			break;
		(void)close(fd);
		fd = -1;
	}
	globfree(&matches);
	if (fd < 0)
		errno = ENODEV;
	return fd;
}

static int set_decode_output_format(int fd, size_t input_length,
				    struct queue_format *result)
{
	struct v4l2_format format;
	uint32_t requested_size;

	if (input_length > UINT32_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	requested_size = input_length < MIN_OUTPUT_BYTES ?
				 MIN_OUTPUT_BYTES :
				 (uint32_t)input_length;
	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
	format.fmt.pix_mp.field = V4L2_FIELD_NONE;
	format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;
	format.fmt.pix_mp.num_planes = 1;
	format.fmt.pix_mp.plane_fmt[0].sizeimage = requested_size;
	if (xioctl(fd, VIDIOC_S_FMT, &format) < 0)
		return -1;
	if (format.type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	    format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_JPEG ||
	    format.fmt.pix_mp.field != V4L2_FIELD_NONE ||
	    format.fmt.pix_mp.colorspace != V4L2_COLORSPACE_JPEG ||
	    format.fmt.pix_mp.num_planes != 1 ||
	    format.fmt.pix_mp.plane_fmt[0].sizeimage < input_length ||
	    format.fmt.pix_mp.plane_fmt[0].sizeimage < MIN_OUTPUT_BYTES ||
	    format.fmt.pix_mp.plane_fmt[0].sizeimage > MAX_JPEG_INPUT_BYTES) {
		errno = EINVAL;
		return -1;
	}
	memset(result, 0, sizeof(*result));
	result->fourcc = format.fmt.pix_mp.pixelformat;
	result->width = format.fmt.pix_mp.width;
	result->height = format.fmt.pix_mp.height;
	result->nplanes = format.fmt.pix_mp.num_planes;
	result->sizeimage[0] = format.fmt.pix_mp.plane_fmt[0].sizeimage;
	return 0;
}

static int set_raw_format(int fd, enum v4l2_buf_type type, uint32_t width,
			  uint32_t height, struct queue_format *result)
{
	struct v4l2_format format;
	unsigned int plane;

	memset(&format, 0, sizeof(format));
	format.type = type;
	format.fmt.pix_mp.width = width;
	format.fmt.pix_mp.height = height;
	format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV16M;
	format.fmt.pix_mp.field = V4L2_FIELD_NONE;
	format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;
	format.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_601;
	format.fmt.pix_mp.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_SRGB;
	format.fmt.pix_mp.num_planes = 2;
	if (xioctl(fd, VIDIOC_S_FMT, &format) < 0)
		return -1;
	if (format.type != type ||
	    format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV16M ||
	    format.fmt.pix_mp.width != width ||
	    format.fmt.pix_mp.height != height ||
	    format.fmt.pix_mp.field != V4L2_FIELD_NONE ||
	    format.fmt.pix_mp.colorspace != V4L2_COLORSPACE_JPEG ||
	    format.fmt.pix_mp.ycbcr_enc != V4L2_YCBCR_ENC_601 ||
	    format.fmt.pix_mp.quantization != V4L2_QUANTIZATION_FULL_RANGE ||
	    format.fmt.pix_mp.xfer_func != V4L2_XFER_FUNC_SRGB ||
	    format.fmt.pix_mp.num_planes != 2) {
		errno = EINVAL;
		return -1;
	}
	memset(result, 0, sizeof(*result));
	result->fourcc = format.fmt.pix_mp.pixelformat;
	result->width = format.fmt.pix_mp.width;
	result->height = format.fmt.pix_mp.height;
	result->nplanes = format.fmt.pix_mp.num_planes;
	for (plane = 0; plane < result->nplanes; ++plane) {
		uint64_t required = (uint64_t)format.fmt.pix_mp.plane_fmt[plane]
					    .bytesperline *
				    height;

		result->stride[plane] =
			format.fmt.pix_mp.plane_fmt[plane].bytesperline;
		result->sizeimage[plane] =
			format.fmt.pix_mp.plane_fmt[plane].sizeimage;
		if (result->stride[plane] < width ||
		    result->sizeimage[plane] < required ||
		    result->sizeimage[plane] > MAX_OUTPUT_BYTES) {
			errno = EINVAL;
			return -1;
		}
	}
	return 0;
}

static int set_encode_quality(int fd, unsigned int requested,
			      unsigned int *actual)
{
	struct v4l2_control control = {
		.id = V4L2_CID_JPEG_COMPRESSION_QUALITY,
		.value = (int32_t)requested,
	};

	if (xioctl(fd, VIDIOC_S_CTRL, &control) < 0)
		return -1;
	control.value = 0;
	if (xioctl(fd, VIDIOC_G_CTRL, &control) < 0)
		return -1;
	if (control.value < 1 || control.value > 100)
		return errno = EINVAL, -1;
	*actual = (unsigned int)control.value;
	return 0;
}

static int set_encode_capture_format(int fd, uint32_t width, uint32_t height,
				     struct queue_format *result)
{
	struct v4l2_format format;

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	format.fmt.pix_mp.width = width;
	format.fmt.pix_mp.height = height;
	format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
	format.fmt.pix_mp.field = V4L2_FIELD_NONE;
	format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;
	format.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_601;
	format.fmt.pix_mp.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_SRGB;
	format.fmt.pix_mp.num_planes = 1;
	format.fmt.pix_mp.plane_fmt[0].sizeimage = MAX_JPEG_INPUT_BYTES;
	if (xioctl(fd, VIDIOC_S_FMT, &format) < 0)
		return -1;
	if (format.type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
	    format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_JPEG ||
	    format.fmt.pix_mp.width != width ||
	    format.fmt.pix_mp.height != height ||
	    format.fmt.pix_mp.field != V4L2_FIELD_NONE ||
	    format.fmt.pix_mp.colorspace != V4L2_COLORSPACE_JPEG ||
	    format.fmt.pix_mp.ycbcr_enc != V4L2_YCBCR_ENC_601 ||
	    format.fmt.pix_mp.quantization != V4L2_QUANTIZATION_FULL_RANGE ||
	    format.fmt.pix_mp.xfer_func != V4L2_XFER_FUNC_SRGB ||
	    format.fmt.pix_mp.num_planes != 1 ||
	    format.fmt.pix_mp.plane_fmt[0].sizeimage < MIN_OUTPUT_BYTES ||
	    format.fmt.pix_mp.plane_fmt[0].sizeimage > MAX_JPEG_INPUT_BYTES) {
		errno = EINVAL;
		return -1;
	}
	memset(result, 0, sizeof(*result));
	result->fourcc = format.fmt.pix_mp.pixelformat;
	result->width = format.fmt.pix_mp.width;
	result->height = format.fmt.pix_mp.height;
	result->nplanes = format.fmt.pix_mp.num_planes;
	result->sizeimage[0] = format.fmt.pix_mp.plane_fmt[0].sizeimage;
	return 0;
}

static void unmap_buffer(struct mapped_buffer *buffer)
{
	unsigned int i;

	for (i = 0; i < buffer->nplanes; ++i)
		if (buffer->addr[i] && buffer->length[i])
			(void)munmap(buffer->addr[i], buffer->length[i]);
	memset(buffer, 0, sizeof(*buffer));
}

static int request_and_map(int fd, enum v4l2_buf_type type,
			   struct mapped_buffer *buffer)
{
	struct v4l2_requestbuffers request;
	struct v4l2_buffer vbuffer;
	struct v4l2_plane planes[MAX_PLANES];
	unsigned int i;

	memset(buffer, 0, sizeof(*buffer));
	memset(&request, 0, sizeof(request));
	request.type = type;
	request.memory = V4L2_MEMORY_MMAP;
	request.count = 1;
	if (xioctl(fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 1) {
		if (request.count < 1)
			errno = ENOMEM;
		return -1;
	}
	memset(&vbuffer, 0, sizeof(vbuffer));
	memset(planes, 0, sizeof(planes));
	vbuffer.type = type;
	vbuffer.memory = V4L2_MEMORY_MMAP;
	vbuffer.index = 0;
	vbuffer.length = MAX_PLANES;
	vbuffer.m.planes = planes;
	if (xioctl(fd, VIDIOC_QUERYBUF, &vbuffer) < 0 || vbuffer.length == 0 ||
	    vbuffer.length > MAX_PLANES) {
		if (vbuffer.length == 0 || vbuffer.length > MAX_PLANES)
			errno = EINVAL;
		return -1;
	}
	buffer->nplanes = vbuffer.length;
	for (i = 0; i < buffer->nplanes; ++i) {
		if (planes[i].length == 0) {
			errno = EINVAL;
			goto fail;
		}
		buffer->length[i] = planes[i].length;
		buffer->addr[i] = mmap(NULL, planes[i].length,
				       PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				       (off_t)planes[i].m.mem_offset);
		if (buffer->addr[i] == MAP_FAILED) {
			buffer->addr[i] = NULL;
			goto fail;
		}
	}
	return 0;

fail:
	unmap_buffer(buffer);
	return -1;
}

static int queue_buffer(int fd, enum v4l2_buf_type type,
			const struct mapped_buffer *buffer,
			const uint32_t bytesused[])
{
	struct v4l2_buffer vbuffer;
	struct v4l2_plane planes[MAX_PLANES];
	unsigned int i;

	memset(&vbuffer, 0, sizeof(vbuffer));
	memset(planes, 0, sizeof(planes));
	if (!buffer->nplanes || buffer->nplanes > MAX_PLANES) {
		errno = EINVAL;
		return -1;
	}
	vbuffer.type = type;
	vbuffer.memory = V4L2_MEMORY_MMAP;
	vbuffer.index = 0;
	vbuffer.length = buffer->nplanes;
	vbuffer.m.planes = planes;
	for (i = 0; i < buffer->nplanes; ++i) {
		if (buffer->length[i] > UINT32_MAX) {
			errno = EOVERFLOW;
			return -1;
		}
		planes[i].length = (uint32_t)buffer->length[i];
		planes[i].bytesused = bytesused ? bytesused[i] : 0;
		if (bytesused && bytesused[i] > buffer->length[i]) {
			errno = EINVAL;
			return -1;
		}
	}
	return xioctl(fd, VIDIOC_QBUF, &vbuffer);
}

static int stream(int fd, enum v4l2_buf_type type, bool enable)
{
	return xioctl(fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
}

static int dequeue_buffer(int fd, enum v4l2_buf_type type,
			  struct v4l2_buffer *vbuffer,
			  struct v4l2_plane planes[MAX_PLANES])
{
	memset(vbuffer, 0, sizeof(*vbuffer));
	memset(planes, 0, sizeof(struct v4l2_plane) * MAX_PLANES);
	vbuffer->type = type;
	vbuffer->memory = V4L2_MEMORY_MMAP;
	vbuffer->length = MAX_PLANES;
	vbuffer->m.planes = planes;
	return xioctl(fd, VIDIOC_DQBUF, vbuffer);
}

static int validate_dequeued(const struct v4l2_buffer *vbuffer,
			     const struct v4l2_plane planes[MAX_PLANES],
			     enum v4l2_buf_type type,
			     const struct mapped_buffer *mapped)
{
	unsigned int plane;

	if (vbuffer->type != type || vbuffer->memory != V4L2_MEMORY_MMAP ||
	    vbuffer->index != 0 || vbuffer->length != mapped->nplanes ||
	    (vbuffer->flags & V4L2_BUF_FLAG_ERROR)) {
		errno = EIO;
		return -1;
	}
	for (plane = 0; plane < mapped->nplanes; ++plane) {
		if (planes[plane].length != mapped->length[plane] ||
		    planes[plane].bytesused > planes[plane].length ||
		    planes[plane].data_offset > planes[plane].bytesused) {
			errno = EIO;
			return -1;
		}
	}
	return 0;
}

static int validate_mapping(const struct mapped_buffer *mapped,
			    const struct queue_format *format)
{
	unsigned int plane;

	if (mapped->nplanes != format->nplanes) {
		errno = EINVAL;
		return -1;
	}
	for (plane = 0; plane < mapped->nplanes; ++plane) {
		if (mapped->length[plane] < format->sizeimage[plane]) {
			errno = EINVAL;
			return -1;
		}
	}
	return 0;
}

static int remaining_timeout(const struct timespec *deadline)
{
	struct timespec now;
	int64_t milliseconds;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -1;
	milliseconds = ((int64_t)deadline->tv_sec - now.tv_sec) * 1000 +
		       ((int64_t)deadline->tv_nsec - now.tv_nsec) / 1000000;
	if (milliseconds <= 0)
		return 0;
	return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static int
wait_for_source_change_or_error(int fd,
				const struct mapped_buffer *output_buffer)
{
	struct timespec deadline;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
		return -1;
	deadline.tv_sec += TIMEOUT_MS / 1000;
	for (;;) {
		struct pollfd pollfd = { .fd = fd, .events = POLLPRI | POLLIN };
		int timeout = remaining_timeout(&deadline);
		int result;

		if (timeout <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		result = poll(&pollfd, 1, timeout);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (result == 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			errno = EIO;
			return -1;
		}
		if (pollfd.revents & POLLPRI) {
			struct v4l2_event event;

			memset(&event, 0, sizeof(event));
			if (xioctl(fd, VIDIOC_DQEVENT, &event) == 0) {
				if (event.type == V4L2_EVENT_SOURCE_CHANGE &&
				    (event.u.src_change.changes &
				     V4L2_EVENT_SRC_CH_RESOLUTION))
					return 0;
			} else if (errno != EAGAIN) {
				return -1;
			}
		}
		if (pollfd.revents & POLLIN) {
			struct v4l2_buffer output;
			struct v4l2_plane planes[MAX_PLANES];

			if (dequeue_buffer(fd,
					   V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
					   &output, planes) == 0) {
				if (validate_dequeued(
					    &output, planes,
					    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
					    output_buffer) < 0)
					return -1;
				errno = EPROTO;
				return -1;
			}
			if (errno != EAGAIN)
				return -1;
		}
	}
}

static int set_decode_scale(int fd, unsigned int factor)
{
	struct v4l2_format format;
	struct v4l2_selection visible;
	struct v4l2_selection compose;
	uint32_t requested_width;
	uint32_t requested_height;

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_G_FMT, &format) < 0)
		return -1;
	memset(&visible, 0, sizeof(visible));
	visible.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	visible.target = V4L2_SEL_TGT_CROP;
	if (xioctl(fd, VIDIOC_G_SELECTION, &visible) < 0)
		return -1;
	if ((format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M &&
	     format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV16M) ||
	    format.fmt.pix_mp.width == 0 || format.fmt.pix_mp.height == 0 ||
	    visible.r.left != 0 || visible.r.top != 0 || visible.r.width <= 0 ||
	    visible.r.height <= 0 ||
	    (uint32_t)visible.r.width != format.fmt.pix_mp.width ||
	    (uint32_t)visible.r.height != format.fmt.pix_mp.height ||
	    (factor == 4 &&
	     (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV16M ||
	      format.fmt.pix_mp.width % factor ||
	      format.fmt.pix_mp.height % factor ||
	      format.fmt.pix_mp.plane_fmt[0].bytesperline !=
		      format.fmt.pix_mp.width ||
	      format.fmt.pix_mp.plane_fmt[0].sizeimage !=
		      (uint64_t)format.fmt.pix_mp.width *
			      format.fmt.pix_mp.height))) {
		errno = EINVAL;
		return -1;
	}
	requested_width = ((uint32_t)visible.r.width + factor - 1U) / factor;
	requested_height = ((uint32_t)visible.r.height + factor - 1U) / factor;
	memset(&compose, 0, sizeof(compose));
	compose.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	compose.target = V4L2_SEL_TGT_COMPOSE;
	compose.r.width = requested_width;
	compose.r.height = requested_height;
	if (xioctl(fd, VIDIOC_S_SELECTION, &compose) < 0)
		return -1;
	if (compose.type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
	    compose.target != V4L2_SEL_TGT_COMPOSE || compose.r.left != 0 ||
	    compose.r.top != 0 || compose.r.width != requested_width ||
	    compose.r.height != requested_height) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int get_capture_layout(int fd, unsigned int decode_factor,
			      struct capture_layout *layout)
{
	struct v4l2_format format;
	struct v4l2_selection selection;

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_G_FMT, &format) < 0)
		return -1;
	if ((format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M &&
	     format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV16M) ||
	    format.fmt.pix_mp.num_planes != 2 || !format.fmt.pix_mp.width ||
	    !format.fmt.pix_mp.height ||
	    format.fmt.pix_mp.field != V4L2_FIELD_NONE ||
	    format.fmt.pix_mp.colorspace != V4L2_COLORSPACE_JPEG ||
	    format.fmt.pix_mp.ycbcr_enc != V4L2_YCBCR_ENC_601 ||
	    format.fmt.pix_mp.quantization != V4L2_QUANTIZATION_FULL_RANGE ||
	    format.fmt.pix_mp.xfer_func != V4L2_XFER_FUNC_SRGB ||
	    !format.fmt.pix_mp.plane_fmt[0].bytesperline ||
	    !format.fmt.pix_mp.plane_fmt[1].bytesperline ||
	    !format.fmt.pix_mp.plane_fmt[0].sizeimage ||
	    !format.fmt.pix_mp.plane_fmt[1].sizeimage ||
	    format.fmt.pix_mp.plane_fmt[0].sizeimage > MAX_OUTPUT_BYTES ||
	    format.fmt.pix_mp.plane_fmt[1].sizeimage > MAX_OUTPUT_BYTES) {
		errno = EINVAL;
		return -1;
	}
	memset(&selection, 0, sizeof(selection));
	selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	selection.target = decode_factor != 1 ? V4L2_SEL_TGT_COMPOSE :
						V4L2_SEL_TGT_CROP;
	if (xioctl(fd, VIDIOC_G_SELECTION, &selection) < 0)
		return -1;
	if (selection.r.left < 0 || selection.r.top < 0 ||
	    selection.r.width <= 0 || selection.r.height <= 0 ||
	    (uint32_t)selection.r.left + (uint32_t)selection.r.width >
		    format.fmt.pix_mp.width ||
	    (uint32_t)selection.r.top + (uint32_t)selection.r.height >
		    format.fmt.pix_mp.height) {
		errno = EINVAL;
		return -1;
	}
	memset(layout, 0, sizeof(*layout));
	layout->fourcc = format.fmt.pix_mp.pixelformat;
	layout->width = (uint32_t)selection.r.width;
	layout->height = (uint32_t)selection.r.height;
	layout->stride[0] = format.fmt.pix_mp.plane_fmt[0].bytesperline;
	layout->stride[1] = format.fmt.pix_mp.plane_fmt[1].bytesperline;
	layout->sizeimage[0] = format.fmt.pix_mp.plane_fmt[0].sizeimage;
	layout->sizeimage[1] = format.fmt.pix_mp.plane_fmt[1].sizeimage;
	layout->crop = selection.r;
	layout->vsub = layout->fourcc == V4L2_PIX_FMT_NV12M ? 2U : 1U;
	layout->decode_factor = decode_factor;
	if (layout->crop.left & 1 ||
	    (unsigned int)layout->crop.top % layout->vsub ||
	    (uint64_t)(uint32_t)layout->crop.left + layout->width >
		    layout->stride[0] ||
	    (uint64_t)(uint32_t)layout->crop.left +
			    (((uint64_t)layout->width + 1U) / 2U) * 2U >
		    layout->stride[1] ||
	    layout->width > UINT32_MAX - 1U) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static void raw_capture_layout(const struct queue_format *format,
			       struct capture_layout *layout)
{
	memset(layout, 0, sizeof(*layout));
	layout->fourcc = format->fourcc;
	layout->width = format->width;
	layout->height = format->height;
	layout->stride[0] = format->stride[0];
	layout->stride[1] = format->stride[1];
	layout->sizeimage[0] = format->sizeimage[0];
	layout->sizeimage[1] = format->sizeimage[1];
	layout->crop.width = (int32_t)format->width;
	layout->crop.height = (int32_t)format->height;
	layout->vsub = 1;
}

static bool same_capture_layout(const struct capture_layout *left,
				const struct capture_layout *right)
{
	return left->fourcc == right->fourcc && left->width == right->width &&
	       left->height == right->height &&
	       left->stride[0] == right->stride[0] &&
	       left->stride[1] == right->stride[1] &&
	       left->sizeimage[0] == right->sizeimage[0] &&
	       left->sizeimage[1] == right->sizeimage[1] &&
	       left->crop.left == right->crop.left &&
	       left->crop.top == right->crop.top &&
	       left->crop.width == right->crop.width &&
	       left->crop.height == right->crop.height &&
	       left->decode_factor == right->decode_factor;
}

static int required_capture_bytes(const struct capture_layout *layout,
				  size_t required[2])
{
	uint64_t y_last;
	uint64_t uv_last;
	uint64_t uv_rows =
		((uint64_t)layout->height + layout->vsub - 1U) / layout->vsub;

	y_last = ((uint64_t)layout->crop.top + layout->height - 1U) *
			 layout->stride[0] +
		 (uint64_t)layout->crop.left + layout->width;
	uv_last = ((uint64_t)layout->crop.top / layout->vsub + uv_rows - 1U) *
			  layout->stride[1] +
		  (uint64_t)layout->crop.left +
		  ((uint64_t)layout->width + 1U) / 2U * 2U;
	if (y_last > SIZE_MAX || uv_last > SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	required[0] = (size_t)y_last;
	required[1] = (size_t)uv_last;
	return 0;
}

static int wait_for_pair(int fd, const struct capture_layout *expected,
			 const struct mapped_buffer *output_buffer,
			 const struct mapped_buffer *capture_buffer,
			 struct v4l2_buffer *capture,
			 struct v4l2_plane capture_planes[MAX_PLANES],
			 struct v4l2_buffer *completed_output,
			 struct v4l2_plane output_planes[MAX_PLANES])
{
	struct timespec deadline;
	bool output_done = false;
	bool capture_done = false;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
		return -1;
	deadline.tv_sec += TIMEOUT_MS / 1000;
	while (!output_done || !capture_done) {
		struct pollfd pollfd = { .fd = fd, .events = POLLPRI | POLLIN };
		int timeout = remaining_timeout(&deadline);
		int result;

		if (timeout <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		result = poll(&pollfd, 1, timeout);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (result == 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			errno = EIO;
			return -1;
		}
		if (pollfd.revents & POLLPRI) {
			struct v4l2_event event;

			memset(&event, 0, sizeof(event));
			if (xioctl(fd, VIDIOC_DQEVENT, &event) == 0) {
				if (event.type == V4L2_EVENT_SOURCE_CHANGE &&
				    (event.u.src_change.changes &
				     V4L2_EVENT_SRC_CH_RESOLUTION)) {
					struct capture_layout current;

					if (!expected ||
					    get_capture_layout(
						    fd, expected->decode_factor,
						    &current) < 0 ||
					    !same_capture_layout(expected,
								 &current)) {
						errno = EPROTO;
						return -1;
					}
				}
			} else if (errno != EAGAIN) {
				return -1;
			}
		}
		if (pollfd.revents & POLLIN) {
			if (!output_done) {
				if (dequeue_buffer(
					    fd,
					    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
					    completed_output,
					    output_planes) == 0) {
					if (validate_dequeued(
						    completed_output,
						    output_planes,
						    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
						    output_buffer) < 0)
						return -1;
					output_done = true;
				} else if (errno != EAGAIN) {
					return -1;
				}
			}
			if (!capture_done) {
				if (dequeue_buffer(
					    fd,
					    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
					    capture, capture_planes) == 0) {
					if (validate_dequeued(
						    capture, capture_planes,
						    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
						    capture_buffer) < 0)
						return -1;
					if (capture->flags &
					    V4L2_BUF_FLAG_LAST) {
						errno = EPIPE;
						return -1;
					}
					capture_done = true;
				} else if (errno != EAGAIN) {
					return -1;
				}
			}
		}
	}
	return 0;
}

static int raw_result_length(const struct capture_layout *layout,
			     size_t *length)
{
	uint64_t uv_rows =
		((uint64_t)layout->height + layout->vsub - 1U) / layout->vsub;
	uint64_t uv_width = ((uint64_t)layout->width + 1U) / 2U * 2U;
	uint64_t total =
		(uint64_t)layout->width * layout->height + uv_width * uv_rows;

	if (total == 0 || total > MAX_OUTPUT_BYTES || total > SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	*length = (size_t)total;
	return 0;
}

static int copy_tight_result(uint8_t *destination, size_t destination_length,
			     const struct mapped_buffer *buffer,
			     const struct capture_layout *layout,
			     const struct v4l2_plane planes[MAX_PLANES])
{
	size_t required[2];
	size_t expected_length;
	size_t offset = 0;
	uint32_t row;
	uint32_t uv_rows;
	uint32_t uv_width;

	if (raw_result_length(layout, &expected_length) < 0 ||
	    required_capture_bytes(layout, required) < 0)
		return -1;
	if (destination_length != expected_length || buffer->nplanes != 2) {
		errno = EINVAL;
		return -1;
	}
	if (planes[0].data_offset > buffer->length[0] ||
	    planes[1].data_offset > buffer->length[1] ||
	    buffer->length[0] - planes[0].data_offset < required[0] ||
	    buffer->length[1] - planes[1].data_offset < required[1] ||
	    planes[0].bytesused - planes[0].data_offset < required[0] ||
	    planes[1].bytesused - planes[1].data_offset < required[1]) {
		errno = EIO;
		return -1;
	}
	for (row = 0; row < layout->height; ++row) {
		const uint8_t *source =
			(const uint8_t *)buffer->addr[0] +
			planes[0].data_offset +
			((size_t)layout->crop.top + row) * layout->stride[0] +
			layout->crop.left;

		memcpy(destination + offset, source, layout->width);
		offset += layout->width;
	}
	uv_rows = (layout->height + layout->vsub - 1U) / layout->vsub;
	uv_width = ((layout->width + 1U) / 2U) * 2U;
	for (row = 0; row < uv_rows; ++row) {
		const uint8_t *source =
			(const uint8_t *)buffer->addr[1] +
			planes[1].data_offset +
			((size_t)layout->crop.top / layout->vsub + row) *
				layout->stride[1] +
			layout->crop.left;

		memcpy(destination + offset, source, uv_width);
		offset += uv_width;
	}
	if (offset != destination_length) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int compare_tight_result(const uint8_t *reference,
				size_t reference_length,
				const struct mapped_buffer *buffer,
				const struct capture_layout *layout,
				const struct v4l2_plane planes[MAX_PLANES])
{
	size_t required[2];
	size_t expected_length;
	size_t offset = 0;
	uint32_t row;
	uint32_t uv_rows;
	uint32_t uv_width;

	if (raw_result_length(layout, &expected_length) < 0 ||
	    required_capture_bytes(layout, required) < 0)
		return -1;
	if (reference_length != expected_length || buffer->nplanes != 2 ||
	    planes[0].data_offset > buffer->length[0] ||
	    planes[1].data_offset > buffer->length[1] ||
	    buffer->length[0] - planes[0].data_offset < required[0] ||
	    buffer->length[1] - planes[1].data_offset < required[1] ||
	    planes[0].bytesused - planes[0].data_offset < required[0] ||
	    planes[1].bytesused - planes[1].data_offset < required[1])
		return errno = EIO, -1;
	for (row = 0; row < layout->height; ++row) {
		const uint8_t *source =
			(const uint8_t *)buffer->addr[0] +
			planes[0].data_offset +
			((size_t)layout->crop.top + row) * layout->stride[0] +
			layout->crop.left;

		if (memcmp(reference + offset, source, layout->width))
			return errno = EIO, -1;
		offset += layout->width;
	}
	uv_rows = (layout->height + layout->vsub - 1U) / layout->vsub;
	uv_width = ((layout->width + 1U) / 2U) * 2U;
	for (row = 0; row < uv_rows; ++row) {
		const uint8_t *source =
			(const uint8_t *)buffer->addr[1] +
			planes[1].data_offset +
			((size_t)layout->crop.top / layout->vsub + row) *
				layout->stride[1] +
			layout->crop.left;

		if (memcmp(reference + offset, source, uv_width))
			return errno = EIO, -1;
		offset += uv_width;
	}
	return 0;
}

static int copy_jpeg_result(uint8_t *destination, size_t destination_capacity,
			    const struct mapped_buffer *buffer,
			    const struct v4l2_plane planes[MAX_PLANES],
			    size_t *length)
{
	size_t payload;

	if (buffer->nplanes != 1 || planes[0].data_offset > buffer->length[0] ||
	    planes[0].bytesused <= planes[0].data_offset) {
		errno = EIO;
		return -1;
	}
	payload = planes[0].bytesused - planes[0].data_offset;
	if (payload > destination_capacity) {
		errno = EOVERFLOW;
		return -1;
	}
	memcpy(destination,
	       (const uint8_t *)buffer->addr[0] + planes[0].data_offset,
	       payload);
	*length = payload;
	return 0;
}

static int copy_raw_input(const uint8_t *input, size_t input_length,
			  const struct queue_format *format,
			  const struct mapped_buffer *buffer)
{
	size_t plane_bytes = (size_t)format->width * format->height;
	unsigned int plane;
	uint32_t row;

	if (format->nplanes != 2 || buffer->nplanes != 2 ||
	    input_length != plane_bytes * 2U)
		return errno = EINVAL, -1;
	for (plane = 0; plane < 2; ++plane) {
		if (buffer->length[plane] < format->sizeimage[plane])
			return errno = EINVAL, -1;
		memset(buffer->addr[plane], 0, buffer->length[plane]);
		for (row = 0; row < format->height; ++row)
			memcpy((uint8_t *)buffer->addr[plane] +
				       (size_t)row * format->stride[plane],
			       input + (size_t)plane * plane_bytes +
				       (size_t)row * format->width,
			       format->width);
	}
	return 0;
}

static int verify_raw_input(const uint8_t *input, size_t input_length,
			    const struct queue_format *format,
			    const struct mapped_buffer *buffer)
{
	size_t plane_bytes = (size_t)format->width * format->height;
	unsigned int plane;
	uint32_t row;

	if (input_length != plane_bytes * 2U)
		return errno = EINVAL, -1;
	for (plane = 0; plane < 2; ++plane)
		for (row = 0; row < format->height; ++row)
			if (memcmp((const uint8_t *)buffer->addr[plane] +
					   (size_t)row * format->stride[plane],
				   input + (size_t)plane * plane_bytes +
					   (size_t)row * format->width,
				   format->width))
				return errno = EIO, -1;
	return 0;
}

static int validate_output_payload(const struct v4l2_plane planes[MAX_PLANES],
				   const uint32_t expected[],
				   unsigned int nplanes)
{
	unsigned int plane;

	for (plane = 0; plane < nplanes; ++plane)
		if (planes[plane].data_offset != 0 ||
		    planes[plane].bytesused != expected[plane])
			return errno = EIO, -1;
	return 0;
}

static uint64_t elapsed_ns(const struct timespec *start,
			   const struct timespec *end)
{
	int64_t seconds = end->tv_sec - start->tv_sec;
	int64_t nanoseconds = end->tv_nsec - start->tv_nsec;

	return (uint64_t)(seconds * 1000000000LL + nanoseconds);
}

static int timed_queue_buffer(int fd, enum v4l2_buf_type type,
			      const struct mapped_buffer *buffer,
			      const uint32_t bytesused[], bool warm,
			      struct fplinux_jpeg_v4l2_timing *timings)
{
	struct timespec started;
	struct timespec done;
	uint64_t duration;

	if (clock_gettime(CLOCK_MONOTONIC, &started) < 0)
		return -1;
	if (queue_buffer(fd, type, buffer, bytesused) < 0)
		return -1;
	if (clock_gettime(CLOCK_MONOTONIC, &done) < 0)
		return -1;
	duration = elapsed_ns(&started, &done);
	timings->queue_ns += duration;
	if (warm)
		timings->warm_queue_ns += duration;
	return 0;
}

int fplinux_jpeg_v4l2_run(const struct fplinux_jpeg_v4l2_request *request,
			  const uint8_t *input, size_t input_length,
			  struct fplinux_jpeg_v4l2_result *result,
			  struct fplinux_jpeg_v4l2_timing *timing, char *error,
			  size_t error_size)
{
	uint8_t *reference = NULL;
	size_t reference_capacity = 0;
	size_t reference_length = 0;
	struct mapped_buffer output = { 0 };
	struct mapped_buffer capture = { 0 };
	struct queue_format output_format = { 0 };
	struct queue_format capture_format = { 0 };
	struct capture_layout layout = { 0 };
	struct v4l2_event_subscription subscription;
	struct v4l2_buffer completed_capture;
	struct v4l2_buffer completed_output;
	struct v4l2_plane completed_capture_planes[MAX_PLANES];
	struct v4l2_plane completed_output_planes[MAX_PLANES];
	uint32_t output_bytes[MAX_PLANES] = { 0 };
	int fd = -1;
	bool output_streaming = false;
	bool capture_streaming = false;
	bool output_requested = false;
	bool capture_requested = false;
	unsigned int iteration;
	unsigned int actual_quality = 0;
	int status = -1;

	memset(result, 0, sizeof(*result));
	fd = open_device(request->device_path, request->operation);
	if (fd < 0) {
		set_error(error, error_size, "device");
		goto out;
	}
	if (request->operation == FPLINUX_JPEG_DECODE) {
		memset(&subscription, 0, sizeof(subscription));
		subscription.type = V4L2_EVENT_SOURCE_CHANGE;
		if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) {
			set_error(error, error_size, "subscribe source-change");
			goto out;
		}
		if (set_decode_output_format(fd, input_length, &output_format) <
			    0 ||
		    request_and_map(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				    &output) < 0 ||
		    validate_mapping(&output, &output_format) < 0) {
			set_error(error, error_size, "output queue");
			goto out;
		}
		output_requested = true;
		memcpy(output.addr[0], input, input_length);
		output_bytes[0] = (uint32_t)input_length;
		if (timed_queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				       &output, output_bytes, false,
				       timing) < 0 ||
		    stream(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true) < 0) {
			set_error(error, error_size, "start output");
			goto out;
		}
		output_streaming = true;
		if (wait_for_source_change_or_error(fd, &output) < 0 ||
		    set_decode_scale(fd, request->scale) < 0 ||
		    get_capture_layout(fd, request->scale, &layout) < 0) {
			set_error(error, error_size, "capture format");
			goto out;
		}
		capture_format.fourcc = layout.fourcc;
		capture_format.width = layout.width;
		capture_format.height = layout.height;
		capture_format.nplanes = 2;
		capture_format.stride[0] = layout.stride[0];
		capture_format.stride[1] = layout.stride[1];
		capture_format.sizeimage[0] = layout.sizeimage[0];
		capture_format.sizeimage[1] = layout.sizeimage[1];
		if (request_and_map(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				    &capture) < 0 ||
		    validate_mapping(&capture, &capture_format) < 0) {
			set_error(error, error_size, "capture queue");
			goto out;
		}
		capture_requested = true;
		if (timed_queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				       &capture, NULL, false, timing) < 0 ||
		    stream(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true) < 0) {
			set_error(error, error_size, "start capture");
			goto out;
		}
		capture_streaming = true;
	} else {
		uint32_t capture_width = request->operation ==
							 FPLINUX_JPEG_SCALE ?
						 request->width / 2U :
						 request->width;
		uint32_t capture_height = request->operation ==
							  FPLINUX_JPEG_SCALE ?
						  request->height / 2U :
						  request->height;

		if (set_raw_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				   request->width, request->height,
				   &output_format) < 0 ||
		    (request->operation == FPLINUX_JPEG_ENCODE ?
			     set_encode_capture_format(fd, capture_width,
						       capture_height,
						       &capture_format) :
			     set_raw_format(fd,
					    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
					    capture_width, capture_height,
					    &capture_format)) < 0) {
			set_error(error, error_size, "formats");
			goto out;
		}
		if (request->operation == FPLINUX_JPEG_ENCODE &&
		    set_encode_quality(fd, request->quality, &actual_quality) <
			    0) {
			set_error(error, error_size, "encode quality");
			goto out;
		}
		if (request_and_map(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				    &output) < 0 ||
		    validate_mapping(&output, &output_format) < 0) {
			set_error(error, error_size, "output queue");
			goto out;
		}
		output_requested = true;
		if (request_and_map(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				    &capture) < 0 ||
		    validate_mapping(&capture, &capture_format) < 0) {
			set_error(error, error_size, "capture queue");
			goto out;
		}
		capture_requested = true;
		if (copy_raw_input(input, input_length, &output_format,
				   &output) < 0) {
			set_error(error, error_size, "raw input");
			goto out;
		}
		output_bytes[0] = output_format.sizeimage[0];
		output_bytes[1] = output_format.sizeimage[1];
		if (timed_queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				       &capture, NULL, false, timing) < 0 ||
		    timed_queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				       &output, output_bytes, false,
				       timing) < 0 ||
		    stream(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true) < 0) {
			set_error(error, error_size, "start queues");
			goto out;
		}
		capture_streaming = true;
		if (stream(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true) < 0) {
			set_error(error, error_size, "start output");
			goto out;
		}
		output_streaming = true;
		if (request->operation != FPLINUX_JPEG_ENCODE)
			raw_capture_layout(&capture_format, &layout);
	}
	if (request->operation == FPLINUX_JPEG_ENCODE)
		reference_capacity = capture_format.sizeimage[0];
	else if (raw_result_length(&layout, &reference_capacity) < 0) {
		set_error(error, error_size, "output geometry");
		goto out;
	}
	reference = malloc(reference_capacity);
	if (!reference) {
		set_error(error, error_size, "output reference");
		goto out;
	}
	if (request->timing &&
	    clock_gettime(CLOCK_MONOTONIC, &timing->prepare_done) < 0) {
		set_error(error, error_size, "timing");
		goto out;
	}
	iteration = 0;
	do {
		struct timespec loop_started;
		struct timespec wait_started;
		struct timespec wait_done;
		struct timespec loop_done;
		uint64_t duration;

		if (clock_gettime(CLOCK_MONOTONIC, &loop_started) < 0) {
			set_error(error, error_size, "timing");
			goto out;
		}
		if (iteration) {
			if (timed_queue_buffer(
				    fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				    &capture, NULL, true, timing) < 0 ||
			    timed_queue_buffer(
				    fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				    &output, output_bytes, true, timing) < 0) {
				set_error(error, error_size, "queue repeat");
				goto out;
			}
		}
		if (clock_gettime(CLOCK_MONOTONIC, &wait_started) < 0 ||
		    wait_for_pair(fd,
				  request->operation == FPLINUX_JPEG_DECODE ?
					  &layout :
					  NULL,
				  &output, &capture, &completed_capture,
				  completed_capture_planes, &completed_output,
				  completed_output_planes) < 0 ||
		    clock_gettime(CLOCK_MONOTONIC, &wait_done) < 0) {
			set_error(error, error_size, "job");
			goto out;
		}
		duration = elapsed_ns(&wait_started, &wait_done);
		timing->wait_ns += duration;
		if (iteration)
			timing->warm_wait_ns += duration;
		if (completed_output.sequence != iteration ||
		    completed_capture.sequence != iteration ||
		    validate_output_payload(completed_output_planes,
					    output_bytes, output.nplanes) < 0) {
			errno = EIO;
			set_error(error, error_size, "completed buffers");
			goto out;
		}
		if (request->operation == FPLINUX_JPEG_DECODE) {
			if (memcmp(output.addr[0], input, input_length)) {
				errno = EIO;
				set_error(error, error_size, "input changed");
				goto out;
			}
		} else if (verify_raw_input(input, input_length, &output_format,
					    &output) < 0) {
			set_error(error, error_size, "input changed");
			goto out;
		}
		if (request->operation == FPLINUX_JPEG_ENCODE) {
			size_t current_length =
				completed_capture_planes[0].bytesused -
				completed_capture_planes[0].data_offset;

			if (!iteration) {
				if (copy_jpeg_result(
					    reference, reference_capacity,
					    &capture, completed_capture_planes,
					    &reference_length) < 0) {
					set_error(error, error_size,
						  "encoded output");
					goto out;
				}
			} else if (current_length != reference_length ||
				   memcmp(reference,
					  (const uint8_t *)capture.addr[0] +
						  completed_capture_planes[0]
							  .data_offset,
					  current_length)) {
				errno = EIO;
				snprintf(error, error_size,
					 "output changed at repeat %u",
					 iteration + 1U);
				goto out;
			}
		} else if (!iteration) {
			reference_length = reference_capacity;
			if (copy_tight_result(reference, reference_length,
					      &capture, &layout,
					      completed_capture_planes) < 0) {
				set_error(error, error_size, "raw output");
				goto out;
			}
		} else if (compare_tight_result(reference, reference_length,
						&capture, &layout,
						completed_capture_planes) < 0) {
			snprintf(error, error_size,
				 "output changed at repeat %u", iteration + 1U);
			goto out;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &loop_done) < 0) {
			set_error(error, error_size, "timing");
			goto out;
		}
		duration = elapsed_ns(&loop_started, &loop_done);
		timing->loop_ns += duration;
		if (iteration)
			timing->warm_loop_ns += duration;
	} while (++iteration < request->repeat);
	result->data = reference;
	result->length = reference_length;
	result->width = layout.width;
	result->height = layout.height;
	result->vsub = layout.vsub;
	result->stride[0] = layout.stride[0];
	result->stride[1] = layout.stride[1];
	result->bytesused[0] = completed_capture_planes[0].bytesused;
	result->bytesused[1] = completed_capture_planes[1].bytesused;
	result->quality = actual_quality;
	reference = NULL;
	status = 0;

out:
	if (capture_streaming || capture_requested)
		(void)stream(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, false);
	if (output_streaming || output_requested)
		(void)stream(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, false);
	unmap_buffer(&capture);
	unmap_buffer(&output);
	if (fd >= 0)
		(void)close(fd);
	free(reference);
	return status;
}
