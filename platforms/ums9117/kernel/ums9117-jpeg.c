// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "ums9117-jpeg-codec.h"
#include "ums9117-jpeg-hw.h"

#define JPEG_NAME "ums9117-jpeg"
#define JPEG_ENCODER_NAME "ums9117-jpeg-enc"
#define JPEG_SCALER_NAME "ums9117-ivsp-scale"

#define JPEG_ENCODER_DEFAULT_CAPACITY UMS9117_JPEG_MAX_INPUT_SIZE

enum jpeg_operation {
	JPEG_OPERATION_DECODE,
	JPEG_OPERATION_ENCODE,
	JPEG_OPERATION_SCALE,
	JPEG_OPERATION_COUNT,
};

struct jpeg_geometry {
	u32 width;
	u32 height;
};

struct jpeg_scale_profile {
	struct jpeg_geometry source;
	struct jpeg_geometry destination;
};

static const struct jpeg_scale_profile
	jpeg_scale_profiles[UMS9117_JPEG_SCALE_PROFILE_COUNT] = {
	[UMS9117_JPEG_SCALE_640X480_TO_320X240] = {
		.source = { 640, 480 },
		.destination = { 320, 240 },
	},
	[UMS9117_JPEG_SCALE_320X240_TO_160X120] = {
		.source = { 320, 240 },
		.destination = { 160, 120 },
	},
};

/* Keep this first-tier proposal isolated until physical qualification trims it. */
static const struct jpeg_geometry jpeg_encoder_geometries[] = {
	{ 1200, 32 },
	{ 320, 240 },
	{ 640, 480 },
};

struct jpeg_buffer {
	struct v4l2_m2m_buffer m2m;
	struct ums9117_jpeg_frame frame;
};

struct jpeg_device;

struct jpeg_video_node {
	struct video_device video;
	struct jpeg_device *jpeg;
	enum jpeg_operation operation;
	const char *card;
};

struct jpeg_context {
	struct v4l2_fh fh;
	struct jpeg_video_node *node;
	struct jpeg_device *jpeg;
	enum jpeg_operation operation;
	enum ums9117_jpeg_scale_profile scale_profile;
	struct v4l2_pix_format_mplane output;
	struct v4l2_pix_format_mplane capture;
	bool header_known;
	bool source_change;
	bool coded_mcu_aligned;
	u32 coded_width;
	u32 coded_height;
	u32 coded_padded_width;
	u32 coded_padded_height;
	u32 coded_fourcc;
	unsigned int decode_factor;
	struct ums9117_jpeg_encode_config encode_config;
	u32 output_sequence;
	u32 capture_sequence;
};

struct jpeg_device {
	struct v4l2_device v4l2;
	struct jpeg_video_node nodes[JPEG_OPERATION_COUNT];
	struct v4l2_m2m_dev *m2m;
	struct ums9117_jpeg_hw *hw;
	struct mutex lock;
	spinlock_t job_lock;
	struct jpeg_context *active;
	struct work_struct work;
};

static struct jpeg_context *jpeg_file_context(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct jpeg_context, fh);
}

static struct jpeg_buffer *jpeg_buffer(struct vb2_v4l2_buffer *buffer)
{
	return container_of(buffer, struct jpeg_buffer, m2m.vb);
}

static struct v4l2_pix_format_mplane *jpeg_format(struct jpeg_context *ctx,
						  enum v4l2_buf_type type)
{
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return &ctx->output;
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return &ctx->capture;
	return NULL;
}

static enum ums9117_jpeg_scale_profile jpeg_scale_output_profile(u32 width,
								 u32 height)
{
	const struct jpeg_scale_profile *small =
		&jpeg_scale_profiles[UMS9117_JPEG_SCALE_320X240_TO_160X120];

	if (width == small->source.width && height == small->source.height)
		return UMS9117_JPEG_SCALE_320X240_TO_160X120;
	return UMS9117_JPEG_SCALE_640X480_TO_320X240;
}

static void jpeg_nv16_format(struct v4l2_pix_format_mplane *pix, u32 width,
			     u32 height)
{
	u32 size = width * height;

	memset(pix, 0, sizeof(*pix));
	pix->width = width;
	pix->height = height;
	pix->pixelformat = V4L2_PIX_FMT_NV16M;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_SRGB;
	pix->num_planes = 2;
	pix->plane_fmt[0].bytesperline = width;
	pix->plane_fmt[0].sizeimage = size;
	pix->plane_fmt[1].bytesperline = width;
	pix->plane_fmt[1].sizeimage = size;
}

static void jpeg_encode_capture_format(struct v4l2_pix_format_mplane *pix,
				       u32 width, u32 height, u32 capacity)
{
	memset(pix, 0, sizeof(*pix));
	pix->width = width;
	pix->height = height;
	pix->pixelformat = V4L2_PIX_FMT_JPEG;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_SRGB;
	pix->num_planes = 1;
	pix->plane_fmt[0].sizeimage = capacity;
}

static void jpeg_decode_capture_format(struct v4l2_pix_format_mplane *pix,
				       const struct ums9117_jpeg_frame *frame,
				       unsigned int factor)
{
	memset(pix, 0, sizeof(*pix));
	pix->width = frame->width / factor;
	pix->height = frame->height / factor;
	pix->pixelformat = frame->vertical_subsampling == 2 ?
				   V4L2_PIX_FMT_NV12M :
				   V4L2_PIX_FMT_NV16M;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_SRGB;
	pix->num_planes = 2;
	pix->plane_fmt[0].bytesperline = frame->padded_width / factor;
	pix->plane_fmt[0].sizeimage =
		frame->padded_width / factor * frame->padded_height / factor;
	pix->plane_fmt[1].bytesperline = frame->padded_width / factor;
	pix->plane_fmt[1].sizeimage =
		pix->plane_fmt[0].sizeimage / frame->vertical_subsampling;
}

static void jpeg_update_decode_capture(struct jpeg_context *ctx)
{
	struct ums9117_jpeg_frame frame = {
		.width = ctx->coded_width,
		.height = ctx->coded_height,
		.padded_width = ctx->coded_padded_width,
		.padded_height = ctx->coded_padded_height,
		.vertical_subsampling =
			ctx->coded_fourcc == V4L2_PIX_FMT_NV12M ? 2 : 1,
	};

	jpeg_decode_capture_format(&ctx->capture, &frame, ctx->decode_factor);
}

static bool jpeg_source_change(struct jpeg_context *ctx,
			       const struct ums9117_jpeg_frame *frame)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};
	struct vb2_v4l2_buffer *last;
	u32 fourcc = frame->vertical_subsampling == 2 ? V4L2_PIX_FMT_NV12M :
							V4L2_PIX_FMT_NV16M;
	bool coded_change;

	coded_change = !ctx->coded_width || ctx->coded_width != frame->width ||
		       ctx->coded_height != frame->height ||
		       ctx->coded_fourcc != fourcc;
	if (ctx->header_known && !coded_change)
		return ctx->source_change;

	if (coded_change)
		ctx->decode_factor = 1;
	ctx->coded_width = frame->width;
	ctx->coded_height = frame->height;
	ctx->coded_padded_width = frame->padded_width;
	ctx->coded_padded_height = frame->padded_height;
	ctx->coded_fourcc = fourcc;
	ctx->coded_mcu_aligned = frame->width == frame->padded_width &&
				 frame->height == frame->padded_height;
	jpeg_update_decode_capture(ctx);
	ctx->header_known = true;
	ctx->source_change = true;
	v4l2_event_queue_fh(&ctx->fh, &event);
	if (vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx))) {
		last = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (last) {
			last->flags |= V4L2_BUF_FLAG_LAST;
			vb2_set_plane_payload(&last->vb2_buf, 0, 0);
			vb2_set_plane_payload(&last->vb2_buf, 1, 0);
			v4l2_m2m_buf_done(last, VB2_BUF_STATE_DONE);
		}
	}
	return true;
}

static int jpeg_querycap(struct file *file, void *priv,
			 struct v4l2_capability *cap)
{
	struct jpeg_context *ctx = jpeg_file_context(file);

	strscpy(cap->driver, JPEG_NAME, sizeof(cap->driver));
	strscpy(cap->card, ctx->node->card, sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" JPEG_NAME, sizeof(cap->bus_info));
	return 0;
}

static int jpeg_enum_decode_format(struct v4l2_fmtdesc *format)
{
	if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		if (format->index)
			return -EINVAL;
		format->pixelformat = V4L2_PIX_FMT_JPEG;
		format->flags = V4L2_FMT_FLAG_COMPRESSED |
				V4L2_FMT_FLAG_DYN_RESOLUTION;
	} else if (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		if (format->index > 1)
			return -EINVAL;
		format->pixelformat = format->index ? V4L2_PIX_FMT_NV16M :
						      V4L2_PIX_FMT_NV12M;
		format->flags = 0;
	} else {
		return -EINVAL;
	}
	return 0;
}

static int jpeg_enum_encode_format(struct v4l2_fmtdesc *format)
{
	if (format->index)
		return -EINVAL;
	if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		format->pixelformat = V4L2_PIX_FMT_NV16M;
		format->flags = 0;
	} else if (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		format->pixelformat = V4L2_PIX_FMT_JPEG;
		format->flags = V4L2_FMT_FLAG_COMPRESSED;
	} else {
		return -EINVAL;
	}
	return 0;
}

static int jpeg_enum_scale_format(struct v4l2_fmtdesc *format)
{
	if (format->index ||
	    (format->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
	     format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE))
		return -EINVAL;
	format->pixelformat = V4L2_PIX_FMT_NV16M;
	format->flags = 0;
	return 0;
}

static int jpeg_enum_format(struct file *file, void *priv,
			    struct v4l2_fmtdesc *format)
{
	struct jpeg_context *ctx = jpeg_file_context(file);

	switch (ctx->operation) {
	case JPEG_OPERATION_DECODE:
		return jpeg_enum_decode_format(format);
	case JPEG_OPERATION_ENCODE:
		return jpeg_enum_encode_format(format);
	case JPEG_OPERATION_SCALE:
		return jpeg_enum_scale_format(format);
	default:
		return -EINVAL;
	}
}

static int jpeg_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *size)
{
	struct jpeg_context *ctx = jpeg_file_context(file);
	const struct jpeg_geometry *geometry;
	const struct jpeg_scale_profile *large =
		&jpeg_scale_profiles[UMS9117_JPEG_SCALE_640X480_TO_320X240];
	const struct jpeg_scale_profile *small =
		&jpeg_scale_profiles[UMS9117_JPEG_SCALE_320X240_TO_160X120];

	switch (ctx->operation) {
	case JPEG_OPERATION_DECODE:
		if (size->index || (size->pixel_format != V4L2_PIX_FMT_JPEG &&
				    size->pixel_format != V4L2_PIX_FMT_NV12M &&
				    size->pixel_format != V4L2_PIX_FMT_NV16M))
			return -EINVAL;
		size->type = V4L2_FRMSIZE_TYPE_STEPWISE;
		size->stepwise.min_width = 1;
		size->stepwise.max_width = UMS9117_JPEG_MAX_DIMENSION;
		size->stepwise.step_width = 1;
		size->stepwise.min_height = 1;
		size->stepwise.max_height = UMS9117_JPEG_MAX_DIMENSION;
		size->stepwise.step_height = 1;
		return 0;
	case JPEG_OPERATION_ENCODE:
		if (size->index >= ARRAY_SIZE(jpeg_encoder_geometries) ||
		    (size->pixel_format != V4L2_PIX_FMT_JPEG &&
		     size->pixel_format != V4L2_PIX_FMT_NV16M))
			return -EINVAL;
		size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		size->discrete.width =
			jpeg_encoder_geometries[size->index].width;
		size->discrete.height =
			jpeg_encoder_geometries[size->index].height;
		return 0;
	case JPEG_OPERATION_SCALE:
		if (size->index > 2 || size->pixel_format != V4L2_PIX_FMT_NV16M)
			return -EINVAL;
		size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		if (!size->index)
			geometry = &large->source;
		else if (size->index == 1)
			geometry = &large->destination;
		else
			geometry = &small->destination;
		size->discrete.width = geometry->width;
		size->discrete.height = geometry->height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int jpeg_get_format(struct file *file, void *priv,
			   struct v4l2_format *format)
{
	struct jpeg_context *ctx = jpeg_file_context(file);
	struct v4l2_pix_format_mplane *pix = jpeg_format(ctx, format->type);

	if (!pix)
		return -EINVAL;
	format->fmt.pix_mp = *pix;
	return 0;
}

static int jpeg_try_decode_format(struct jpeg_context *ctx,
				  struct v4l2_format *format)
{
	struct v4l2_pix_format_mplane *pix = &format->fmt.pix_mp;
	u32 bytes;

	if (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		*pix = ctx->capture;
		return 0;
	}
	if (format->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;
	bytes = clamp_t(u32, pix->plane_fmt[0].sizeimage, 4096,
			UMS9117_JPEG_MAX_INPUT_SIZE);
	memset(pix, 0, sizeof(*pix));
	pix->pixelformat = V4L2_PIX_FMT_JPEG;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;
	pix->num_planes = 1;
	pix->plane_fmt[0].sizeimage = bytes;
	return 0;
}

static const struct jpeg_geometry *jpeg_nearest_encode_geometry(u32 width,
								u32 height)
{
	const struct jpeg_geometry *best = &jpeg_encoder_geometries[0];
	u64 best_distance = ~0ULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(jpeg_encoder_geometries); i++) {
		const struct jpeg_geometry *geometry =
			&jpeg_encoder_geometries[i];
		u64 distance = (u64)(geometry->width > width ?
					     geometry->width - width :
					     width - geometry->width) +
			       (u64)(geometry->height > height ?
					     geometry->height - height :
					     height - geometry->height);

		if (distance < best_distance) {
			best = geometry;
			best_distance = distance;
		}
	}
	return best;
}

static int jpeg_try_encode_format(struct jpeg_context *ctx,
				  struct v4l2_format *format)
{
	struct v4l2_pix_format_mplane *pix = &format->fmt.pix_mp;
	const struct jpeg_geometry *geometry;
	u32 capacity;

	if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		geometry =
			jpeg_nearest_encode_geometry(pix->width, pix->height);
		jpeg_nv16_format(pix, geometry->width, geometry->height);
		return 0;
	}
	if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;
	capacity = clamp_t(u32, pix->plane_fmt[0].sizeimage, 4096,
			   UMS9117_JPEG_MAX_INPUT_SIZE);
	jpeg_encode_capture_format(pix, ctx->output.width, ctx->output.height,
				   capacity);
	return 0;
}

static int jpeg_try_scale_format(struct jpeg_context *ctx,
				 struct v4l2_format *format)
{
	const struct jpeg_scale_profile *profile;

	if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		profile = &jpeg_scale_profiles[jpeg_scale_output_profile(
			format->fmt.pix_mp.width, format->fmt.pix_mp.height)];
		jpeg_nv16_format(&format->fmt.pix_mp, profile->source.width,
				 profile->source.height);
		return 0;
	}
	if (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		profile = &jpeg_scale_profiles[ctx->scale_profile];
		jpeg_nv16_format(&format->fmt.pix_mp,
				 profile->destination.width,
				 profile->destination.height);
		return 0;
	}
	return -EINVAL;
}

static int jpeg_try_format(struct file *file, void *priv,
			   struct v4l2_format *format)
{
	struct jpeg_context *ctx = jpeg_file_context(file);

	switch (ctx->operation) {
	case JPEG_OPERATION_DECODE:
		return jpeg_try_decode_format(ctx, format);
	case JPEG_OPERATION_ENCODE:
		return jpeg_try_encode_format(ctx, format);
	case JPEG_OPERATION_SCALE:
		return jpeg_try_scale_format(ctx, format);
	default:
		return -EINVAL;
	}
}

static int jpeg_set_scale_format(struct jpeg_context *ctx,
				 struct v4l2_format *format)
{
	const struct jpeg_scale_profile *profile;
	struct vb2_queue *queue;
	enum ums9117_jpeg_scale_profile selected;

	if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		queue = v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
		if (vb2_is_busy(queue) ||
		    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
			return -EBUSY;
		selected = jpeg_scale_output_profile(format->fmt.pix_mp.width,
						     format->fmt.pix_mp.height);
		profile = &jpeg_scale_profiles[selected];
		jpeg_nv16_format(&format->fmt.pix_mp, profile->source.width,
				 profile->source.height);
		ctx->scale_profile = selected;
		ctx->output = format->fmt.pix_mp;
		jpeg_nv16_format(&ctx->capture, profile->destination.width,
				 profile->destination.height);
		return 0;
	}
	if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;
	queue = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	if (vb2_is_busy(queue))
		return -EBUSY;
	profile = &jpeg_scale_profiles[ctx->scale_profile];
	jpeg_nv16_format(&format->fmt.pix_mp, profile->destination.width,
			 profile->destination.height);
	ctx->capture = format->fmt.pix_mp;
	return 0;
}

static int jpeg_set_format(struct file *file, void *priv,
			   struct v4l2_format *format)
{
	struct jpeg_context *ctx = jpeg_file_context(file);
	struct vb2_queue *queue;
	struct v4l2_pix_format_mplane *pix;
	u32 capacity;
	int ret;

	if (ctx->operation == JPEG_OPERATION_SCALE)
		return jpeg_set_scale_format(ctx, format);
	pix = jpeg_format(ctx, format->type);
	if (!pix)
		return -EINVAL;
	queue = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, format->type);
	if (vb2_is_busy(queue))
		return -EBUSY;
	if (ctx->operation == JPEG_OPERATION_ENCODE &&
	    format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
	    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	ret = jpeg_try_format(file, priv, format);
	if (ret)
		return ret;
	*pix = format->fmt.pix_mp;

	if (ctx->operation == JPEG_OPERATION_DECODE &&
	    V4L2_TYPE_IS_OUTPUT(format->type)) {
		ctx->header_known = false;
		ctx->source_change = false;
	} else if (ctx->operation == JPEG_OPERATION_ENCODE &&
		   V4L2_TYPE_IS_OUTPUT(format->type)) {
		capacity = ctx->capture.plane_fmt[0].sizeimage;
		jpeg_encode_capture_format(&ctx->capture, ctx->output.width,
					   ctx->output.height, capacity);
	}
	return 0;
}

static int jpeg_get_selection(struct file *file, void *priv,
			      struct v4l2_selection *selection)
{
	struct jpeg_context *ctx = jpeg_file_context(file);
	u32 width;
	u32 height;

	if (ctx->operation == JPEG_OPERATION_DECODE) {
		if (selection->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
		    selection->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			return -EINVAL;
		switch (selection->target) {
		case V4L2_SEL_TGT_CROP:
		case V4L2_SEL_TGT_CROP_DEFAULT:
		case V4L2_SEL_TGT_CROP_BOUNDS:
			width = ctx->coded_width ? ctx->coded_width :
						   ctx->capture.width;
			height = ctx->coded_height ? ctx->coded_height :
						     ctx->capture.height;
			break;
		case V4L2_SEL_TGT_COMPOSE:
		case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		case V4L2_SEL_TGT_COMPOSE_BOUNDS:
			width = ctx->capture.width;
			height = ctx->capture.height;
			break;
		default:
			return -EINVAL;
		}
	} else if (ctx->operation == JPEG_OPERATION_SCALE) {
		switch (selection->target) {
		case V4L2_SEL_TGT_CROP:
		case V4L2_SEL_TGT_CROP_DEFAULT:
		case V4L2_SEL_TGT_CROP_BOUNDS:
			if (selection->type != V4L2_BUF_TYPE_VIDEO_OUTPUT &&
			    selection->type !=
				    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
				return -EINVAL;
			width = ctx->output.width;
			height = ctx->output.height;
			break;
		case V4L2_SEL_TGT_COMPOSE:
		case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		case V4L2_SEL_TGT_COMPOSE_BOUNDS:
			if (selection->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
			    selection->type !=
				    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
				return -EINVAL;
			width = ctx->capture.width;
			height = ctx->capture.height;
			break;
		default:
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	selection->r.left = 0;
	selection->r.top = 0;
	selection->r.width = width;
	selection->r.height = height;
	return 0;
}

static int jpeg_set_selection(struct file *file, void *priv,
			      struct v4l2_selection *selection)
{
	struct jpeg_context *ctx = jpeg_file_context(file);
	struct vb2_queue *capture_queue;
	unsigned int factor;

	if (ctx->operation != JPEG_OPERATION_DECODE ||
	    selection->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    selection->target != V4L2_SEL_TGT_COMPOSE || !ctx->header_known)
		return -EINVAL;
	capture_queue = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	if (vb2_is_busy(capture_queue))
		return -EBUSY;
	if (selection->r.left || selection->r.top)
		return -EINVAL;
	if (selection->r.width == ctx->coded_width &&
	    selection->r.height == ctx->coded_height) {
		factor = 1;
	} else if (selection->r.width == ctx->coded_width / 4 &&
		   selection->r.height == ctx->coded_height / 4) {
		if (ctx->coded_fourcc != V4L2_PIX_FMT_NV16M ||
		    !ctx->coded_mcu_aligned)
			return -EINVAL;
		factor = 4;
	} else {
		return -EINVAL;
	}

	ctx->decode_factor = factor;
	jpeg_update_decode_capture(ctx);
	selection->r.width = ctx->capture.width;
	selection->r.height = ctx->capture.height;
	return 0;
}

static int
jpeg_subscribe_event(struct v4l2_fh *fh,
		     const struct v4l2_event_subscription *subscription)
{
	struct jpeg_context *ctx = container_of(fh, struct jpeg_context, fh);

	if (ctx->operation != JPEG_OPERATION_DECODE ||
	    subscription->type != V4L2_EVENT_SOURCE_CHANGE)
		return -EINVAL;
	return v4l2_event_subscribe(fh, subscription, 4, NULL);
}

static int jpeg_queue_setup(struct vb2_queue *queue, unsigned int *buffers,
			    unsigned int *planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct jpeg_context *ctx = vb2_get_drv_priv(queue);
	struct v4l2_pix_format_mplane *pix = jpeg_format(ctx, queue->type);
	unsigned int plane;

	if (*planes) {
		if (*planes != pix->num_planes)
			return -EINVAL;
		for (plane = 0; plane < *planes; plane++)
			if (sizes[plane] < pix->plane_fmt[plane].sizeimage)
				return -EINVAL;
		return 0;
	}
	*planes = pix->num_planes;
	for (plane = 0; plane < *planes; plane++)
		sizes[plane] = pix->plane_fmt[plane].sizeimage;
	return 0;
}

static int jpeg_raw_buffer_prepare(struct vb2_buffer *buffer,
				   struct v4l2_pix_format_mplane *pix,
				   bool source)
{
	unsigned int plane;

	for (plane = 0; plane < pix->num_planes; plane++) {
		if (buffer->planes[plane].data_offset ||
		    vb2_plane_size(buffer, plane) <
			    pix->plane_fmt[plane].sizeimage)
			return -EINVAL;
		if (source) {
			if (vb2_get_plane_payload(buffer, plane) !=
			    pix->plane_fmt[plane].sizeimage)
				return -EINVAL;
		} else {
			vb2_set_plane_payload(buffer, plane, 0);
		}
	}
	return 0;
}

static int jpeg_buffer_prepare(struct vb2_buffer *buffer)
{
	struct jpeg_context *ctx = vb2_get_drv_priv(buffer->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(buffer);
	struct v4l2_pix_format_mplane *pix;
	size_t size;
	unsigned int plane;

	vbuf->field = V4L2_FIELD_NONE;
	if (ctx->operation == JPEG_OPERATION_DECODE) {
		if (V4L2_TYPE_IS_OUTPUT(buffer->vb2_queue->type)) {
			size = vb2_get_plane_payload(buffer, 0);
			if (buffer->planes[0].data_offset || !size ||
			    size > vb2_plane_size(buffer, 0) ||
			    size > UMS9117_JPEG_MAX_INPUT_SIZE)
				return -EINVAL;
			return ums9117_jpeg_parse(vb2_plane_vaddr(buffer, 0),
						  size,
						  &jpeg_buffer(vbuf)->frame);
		}
		pix = &ctx->capture;
		for (plane = 0; plane < pix->num_planes; plane++) {
			if (vb2_plane_size(buffer, plane) <
			    pix->plane_fmt[plane].sizeimage)
				return -EINVAL;
			vb2_set_plane_payload(buffer, plane, 0);
		}
		return 0;
	}

	if (ctx->operation == JPEG_OPERATION_ENCODE &&
	    V4L2_TYPE_IS_CAPTURE(buffer->vb2_queue->type)) {
		if (buffer->planes[0].data_offset ||
		    vb2_plane_size(buffer, 0) <
			    ctx->capture.plane_fmt[0].sizeimage)
			return -EINVAL;
		vb2_set_plane_payload(buffer, 0, 0);
		return 0;
	}
	pix = jpeg_format(ctx, buffer->vb2_queue->type);
	return jpeg_raw_buffer_prepare(
		buffer, pix, V4L2_TYPE_IS_OUTPUT(buffer->vb2_queue->type));
}

static void jpeg_buffer_queue(struct vb2_buffer *buffer)
{
	struct jpeg_context *ctx = vb2_get_drv_priv(buffer->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(buffer);

	if (ctx->operation == JPEG_OPERATION_DECODE &&
	    V4L2_TYPE_IS_OUTPUT(buffer->vb2_queue->type) &&
	    !v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx))
		jpeg_source_change(ctx, &jpeg_buffer(vbuf)->frame);
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void jpeg_return_buffers(struct vb2_queue *queue,
				enum vb2_buffer_state state)
{
	struct jpeg_context *ctx = vb2_get_drv_priv(queue);
	struct vb2_v4l2_buffer *buffer;

	for (;;) {
		buffer = V4L2_TYPE_IS_OUTPUT(queue->type) ?
				 v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
				 v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!buffer)
			break;
		v4l2_m2m_buf_done(buffer, state);
	}
}

static int jpeg_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct jpeg_context *ctx = vb2_get_drv_priv(queue);

	if (ums9117_jpeg_hw_failed(ctx->jpeg->hw) ||
	    (ctx->operation == JPEG_OPERATION_DECODE &&
	     V4L2_TYPE_IS_CAPTURE(queue->type) && !ctx->header_known)) {
		jpeg_return_buffers(queue, VB2_BUF_STATE_QUEUED);
		return -EIO;
	}
	if (V4L2_TYPE_IS_CAPTURE(queue->type)) {
		if (ctx->operation == JPEG_OPERATION_DECODE)
			ctx->source_change = false;
		ctx->capture_sequence = 0;
	} else {
		ctx->output_sequence = 0;
	}
	return 0;
}

static void jpeg_stop_streaming(struct vb2_queue *queue)
{
	jpeg_return_buffers(queue, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops jpeg_queue_ops = {
	.queue_setup = jpeg_queue_setup,
	.buf_prepare = jpeg_buffer_prepare,
	.buf_queue = jpeg_buffer_queue,
	.start_streaming = jpeg_start_streaming,
	.stop_streaming = jpeg_stop_streaming,
};

static int jpeg_queue_init(void *priv, struct vb2_queue *source,
			   struct vb2_queue *destination)
{
	struct jpeg_context *ctx = priv;
	struct vb2_queue *queues[] = { source, destination };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(queues); i++) {
		struct vb2_queue *queue = queues[i];

		queue->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
				  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		queue->io_modes = VB2_MMAP;
		queue->drv_priv = ctx;
		queue->buf_struct_size = sizeof(struct jpeg_buffer);
		queue->ops = &jpeg_queue_ops;
		queue->mem_ops = &vb2_vmalloc_memops;
		queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		queue->lock = &ctx->jpeg->lock;
		queue->dev = ctx->jpeg->v4l2.dev;
		ret = vb2_queue_init(queue);
		if (ret)
			return ret;
	}
	return 0;
}

static int jpeg_run_decode(struct jpeg_context *ctx,
			   struct vb2_v4l2_buffer *source,
			   struct vb2_v4l2_buffer *destination)
{
	struct ums9117_jpeg_frame *frame = &jpeg_buffer(source)->frame;
	u8 *output[2];
	size_t capacity[2];
	unsigned int plane;
	int ret;

	for (plane = 0; plane < 2; plane++) {
		output[plane] = vb2_plane_vaddr(&destination->vb2_buf, plane);
		capacity[plane] = vb2_plane_size(&destination->vb2_buf, plane);
	}
	ret = ums9117_jpeg_hw_decode(ctx->jpeg->hw, frame,
				     (u8 *)vb2_plane_vaddr(&source->vb2_buf,
							   0) +
					     frame->entropy_offset,
				     output, capacity, ctx->decode_factor);
	if (ret)
		return ret;
	for (plane = 0; plane < 2; plane++)
		vb2_set_plane_payload(&destination->vb2_buf, plane,
				      ctx->capture.plane_fmt[plane].sizeimage);
	return 0;
}

static int jpeg_run_encode(struct jpeg_context *ctx,
			   struct vb2_v4l2_buffer *source,
			   struct vb2_v4l2_buffer *destination)
{
	const u8 *input[2];
	const size_t input_size[2] = {
		ctx->output.plane_fmt[0].sizeimage,
		ctx->output.plane_fmt[1].sizeimage,
	};
	u8 *output = vb2_plane_vaddr(&destination->vb2_buf, 0);
	size_t capacity = ctx->capture.plane_fmt[0].sizeimage;
	size_t written = 0;
	int ret;

	input[0] = vb2_plane_vaddr(&source->vb2_buf, 0);
	input[1] = vb2_plane_vaddr(&source->vb2_buf, 1);
	ret = ums9117_jpeg_build_encode_config(
		ctx->output.width, ctx->output.height, &ctx->encode_config);
	if (ret)
		return ret;
	ret = ums9117_jpeg_hw_encode(ctx->jpeg->hw, &ctx->encode_config, input,
				     input_size, output, capacity, &written);
	if (ret)
		return ret;
	if (!written || written > capacity)
		return -EIO;
	vb2_set_plane_payload(&destination->vb2_buf, 0, written);
	return 0;
}

static int jpeg_run_scale(struct jpeg_context *ctx,
			  struct vb2_v4l2_buffer *source,
			  struct vb2_v4l2_buffer *destination)
{
	const u8 *input[2];
	const size_t input_size[2] = {
		ctx->output.plane_fmt[0].sizeimage,
		ctx->output.plane_fmt[1].sizeimage,
	};
	u8 *output[2];
	const size_t capacity[2] = {
		ctx->capture.plane_fmt[0].sizeimage,
		ctx->capture.plane_fmt[1].sizeimage,
	};
	unsigned int plane;
	int ret;

	for (plane = 0; plane < 2; plane++) {
		input[plane] = vb2_plane_vaddr(&source->vb2_buf, plane);
		output[plane] = vb2_plane_vaddr(&destination->vb2_buf, plane);
	}
	ret = ums9117_jpeg_hw_scale(ctx->jpeg->hw, ctx->scale_profile, input,
				    input_size, output, capacity);
	if (ret)
		return ret;
	for (plane = 0; plane < 2; plane++)
		vb2_set_plane_payload(&destination->vb2_buf, plane,
				      ctx->capture.plane_fmt[plane].sizeimage);
	return 0;
}

static void jpeg_work(struct work_struct *work)
{
	struct jpeg_device *jpeg = container_of(work, struct jpeg_device, work);
	struct jpeg_context *ctx = READ_ONCE(jpeg->active);
	struct vb2_v4l2_buffer *source, *destination;
	enum vb2_buffer_state state = VB2_BUF_STATE_ERROR;
	unsigned long flags;
	int ret;

	source = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	destination = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!source || !destination)
		goto finish;
	switch (ctx->operation) {
	case JPEG_OPERATION_DECODE:
		ret = jpeg_run_decode(ctx, source, destination);
		break;
	case JPEG_OPERATION_ENCODE:
		ret = jpeg_run_encode(ctx, source, destination);
		break;
	case JPEG_OPERATION_SCALE:
		ret = jpeg_run_scale(ctx, source, destination);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret)
		state = VB2_BUF_STATE_DONE;
	v4l2_m2m_buf_copy_metadata(source, destination, true);
	source->sequence = ctx->output_sequence++;
	destination->sequence = ctx->capture_sequence++;

finish:
	source = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	destination = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (source)
		v4l2_m2m_buf_done(source, state);
	if (destination)
		v4l2_m2m_buf_done(destination, state);
	spin_lock_irqsave(&jpeg->job_lock, flags);
	jpeg->active = NULL;
	spin_unlock_irqrestore(&jpeg->job_lock, flags);
	v4l2_m2m_job_finish(jpeg->m2m, ctx->fh.m2m_ctx);
}

static int jpeg_job_ready(void *priv)
{
	struct jpeg_context *ctx = priv;

	return ctx->operation != JPEG_OPERATION_DECODE || !ctx->source_change;
}

static void jpeg_device_run(void *priv)
{
	struct jpeg_context *ctx = priv;
	struct jpeg_device *jpeg = ctx->jpeg;
	struct vb2_v4l2_buffer *source;
	unsigned long flags;

	if (ctx->operation == JPEG_OPERATION_DECODE) {
		source = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
		if (source &&
		    jpeg_source_change(ctx, &jpeg_buffer(source)->frame)) {
			v4l2_m2m_job_finish(jpeg->m2m, ctx->fh.m2m_ctx);
			return;
		}
	}
	spin_lock_irqsave(&jpeg->job_lock, flags);
	ums9117_jpeg_hw_prepare(jpeg->hw);
	jpeg->active = ctx;
	spin_unlock_irqrestore(&jpeg->job_lock, flags);
	schedule_work(&jpeg->work);
}

static void jpeg_job_abort(void *priv)
{
	struct jpeg_context *ctx = priv;
	struct jpeg_device *jpeg = ctx->jpeg;
	unsigned long flags;
	bool active;

	spin_lock_irqsave(&jpeg->job_lock, flags);
	active = jpeg->active == ctx;
	if (active)
		ums9117_jpeg_hw_cancel(jpeg->hw);
	spin_unlock_irqrestore(&jpeg->job_lock, flags);
	if (active)
		flush_work(&jpeg->work);
}

static const struct v4l2_m2m_ops jpeg_m2m_ops = {
	.device_run = jpeg_device_run,
	.job_ready = jpeg_job_ready,
	.job_abort = jpeg_job_abort,
};

static void jpeg_init_decode_formats(struct jpeg_context *ctx)
{
	ctx->decode_factor = 1;
	ctx->output.pixelformat = V4L2_PIX_FMT_JPEG;
	ctx->output.field = V4L2_FIELD_NONE;
	ctx->output.num_planes = 1;
	ctx->output.plane_fmt[0].sizeimage = 65536;
	ctx->capture.width = 320;
	ctx->capture.height = 240;
	ctx->capture.pixelformat = V4L2_PIX_FMT_NV12M;
	ctx->capture.field = V4L2_FIELD_NONE;
	ctx->capture.colorspace = V4L2_COLORSPACE_JPEG;
	ctx->capture.num_planes = 2;
	ctx->capture.plane_fmt[0].bytesperline = 320;
	ctx->capture.plane_fmt[0].sizeimage = 320 * 240;
	ctx->capture.plane_fmt[1].bytesperline = 320;
	ctx->capture.plane_fmt[1].sizeimage = 320 * 120;
}

static void jpeg_init_encode_formats(struct jpeg_context *ctx)
{
	jpeg_nv16_format(&ctx->output, jpeg_encoder_geometries[0].width,
			 jpeg_encoder_geometries[0].height);
	jpeg_encode_capture_format(&ctx->capture, ctx->output.width,
				   ctx->output.height,
				   JPEG_ENCODER_DEFAULT_CAPACITY);
}

static void jpeg_init_scale_formats(struct jpeg_context *ctx)
{
	const struct jpeg_scale_profile *profile;

	ctx->scale_profile = UMS9117_JPEG_SCALE_640X480_TO_320X240;
	profile = &jpeg_scale_profiles[ctx->scale_profile];
	jpeg_nv16_format(&ctx->output, profile->source.width,
			 profile->source.height);
	jpeg_nv16_format(&ctx->capture, profile->destination.width,
			 profile->destination.height);
}

static int jpeg_open(struct file *file)
{
	struct jpeg_video_node *node = video_drvdata(file);
	struct jpeg_device *jpeg = node->jpeg;
	struct jpeg_context *ctx;
	int ret;

	if (mutex_lock_interruptible(&jpeg->lock))
		return -ERESTARTSYS;
	if (ums9117_jpeg_hw_failed(jpeg->hw)) {
		ret = -EIO;
		goto unlock;
	}
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto unlock;
	}
	ctx->node = node;
	ctx->jpeg = jpeg;
	ctx->operation = node->operation;
	switch (ctx->operation) {
	case JPEG_OPERATION_DECODE:
		jpeg_init_decode_formats(ctx);
		break;
	case JPEG_OPERATION_ENCODE:
		jpeg_init_encode_formats(ctx);
		break;
	case JPEG_OPERATION_SCALE:
		jpeg_init_scale_formats(ctx);
		break;
	default:
		ret = -EINVAL;
		goto free_context;
	}
	v4l2_fh_init(&ctx->fh, &node->video);
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(jpeg->m2m, ctx, jpeg_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		goto free_context;
	}
	v4l2_fh_add(&ctx->fh, file);
	ret = 0;
	goto unlock;

free_context:
	kfree(ctx);
unlock:
	mutex_unlock(&jpeg->lock);
	return ret;
}

static int jpeg_release(struct file *file)
{
	struct jpeg_context *ctx = jpeg_file_context(file);
	struct jpeg_device *jpeg = ctx->jpeg;

	mutex_lock(&jpeg->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&jpeg->lock);
	return 0;
}

static const struct v4l2_file_operations jpeg_file_ops = {
	.owner = THIS_MODULE,
	.open = jpeg_open,
	.release = jpeg_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct v4l2_ioctl_ops jpeg_ioctl_ops = {
	.vidioc_querycap = jpeg_querycap,
	.vidioc_enum_framesizes = jpeg_enum_framesizes,
	.vidioc_enum_fmt_vid_out = jpeg_enum_format,
	.vidioc_g_fmt_vid_out_mplane = jpeg_get_format,
	.vidioc_try_fmt_vid_out_mplane = jpeg_try_format,
	.vidioc_s_fmt_vid_out_mplane = jpeg_set_format,
	.vidioc_enum_fmt_vid_cap = jpeg_enum_format,
	.vidioc_g_fmt_vid_cap_mplane = jpeg_get_format,
	.vidioc_try_fmt_vid_cap_mplane = jpeg_try_format,
	.vidioc_s_fmt_vid_cap_mplane = jpeg_set_format,
	.vidioc_g_selection = jpeg_get_selection,
	.vidioc_s_selection = jpeg_set_selection,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_subscribe_event = jpeg_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

struct jpeg_node_info {
	const char *name;
	const char *card;
};

static const struct jpeg_node_info jpeg_node_info[JPEG_OPERATION_COUNT] = {
	[JPEG_OPERATION_DECODE] = {
		.name = JPEG_NAME,
		.card = "UMS9117 JPEG decoder",
	},
	[JPEG_OPERATION_ENCODE] = {
		.name = JPEG_ENCODER_NAME,
		.card = "UMS9117 JPEG encoder",
	},
	[JPEG_OPERATION_SCALE] = {
		.name = JPEG_SCALER_NAME,
		.card = "UMS9117 RAM scaler",
	},
};

static int jpeg_probe(struct platform_device *pdev)
{
	struct jpeg_device *jpeg;
	unsigned int i;
	int ret;

	jpeg = devm_kzalloc(&pdev->dev, sizeof(*jpeg), GFP_KERNEL);
	if (!jpeg)
		return -ENOMEM;
	mutex_init(&jpeg->lock);
	spin_lock_init(&jpeg->job_lock);
	INIT_WORK(&jpeg->work, jpeg_work);
	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to attach JPEG DMA pool\n");
	jpeg->hw = ums9117_jpeg_hw_create(pdev);
	if (IS_ERR(jpeg->hw)) {
		ret = PTR_ERR(jpeg->hw);
		goto release_memory;
	}
	ret = v4l2_device_register(&pdev->dev, &jpeg->v4l2);
	if (ret)
		goto destroy_hw;
	jpeg->m2m = v4l2_m2m_init(&jpeg_m2m_ops);
	if (IS_ERR(jpeg->m2m)) {
		ret = PTR_ERR(jpeg->m2m);
		goto unregister_v4l2;
	}

	for (i = 0; i < JPEG_OPERATION_COUNT; i++) {
		struct jpeg_video_node *node = &jpeg->nodes[i];

		node->jpeg = jpeg;
		node->operation = (enum jpeg_operation)i;
		node->card = jpeg_node_info[i].card;
		node->video.fops = &jpeg_file_ops;
		node->video.ioctl_ops = &jpeg_ioctl_ops;
		node->video.v4l2_dev = &jpeg->v4l2;
		node->video.lock = &jpeg->lock;
		node->video.release = video_device_release_empty;
		node->video.vfl_dir = VFL_DIR_M2M;
		node->video.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE |
					  V4L2_CAP_STREAMING;
		strscpy(node->video.name, jpeg_node_info[i].name,
			sizeof(node->video.name));
		video_set_drvdata(&node->video, node);
		ret = video_register_device(&node->video, VFL_TYPE_VIDEO, -1);
		if (ret)
			goto unregister_nodes;
		dev_info(&pdev->dev, "%s registered as /dev/video%d\n",
			 node->card, node->video.num);
	}
	platform_set_drvdata(pdev, jpeg);
	return 0;

unregister_nodes:
	while (i) {
		i--;
		video_unregister_device(&jpeg->nodes[i].video);
	}
	ums9117_jpeg_hw_cancel(jpeg->hw);
	flush_work(&jpeg->work);
	v4l2_m2m_release(jpeg->m2m);
unregister_v4l2:
	v4l2_device_unregister(&jpeg->v4l2);
destroy_hw:
	ums9117_jpeg_hw_destroy(jpeg->hw);
release_memory:
	of_reserved_mem_device_release(&pdev->dev);
	return ret;
}

static void jpeg_remove(struct platform_device *pdev)
{
	struct jpeg_device *jpeg = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = JPEG_OPERATION_COUNT; i > 0; i--)
		video_unregister_device(&jpeg->nodes[i - 1].video);
	ums9117_jpeg_hw_cancel(jpeg->hw);
	flush_work(&jpeg->work);
	v4l2_m2m_release(jpeg->m2m);
	v4l2_device_unregister(&jpeg->v4l2);
	ums9117_jpeg_hw_destroy(jpeg->hw);
	of_reserved_mem_device_release(&pdev->dev);
}

static const struct of_device_id jpeg_of_match[] = {
	{ .compatible = "sprd,ums9117-jpeg" },
	{}
};
MODULE_DEVICE_TABLE(of, jpeg_of_match);

static struct platform_driver jpeg_driver = {
	.probe = jpeg_probe,
	.remove = jpeg_remove,
	.driver = {
		.name = JPEG_NAME,
		.of_match_table = jpeg_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(jpeg_driver);

MODULE_DESCRIPTION("UMS9117 JPEG and RAM memory engines");
MODULE_LICENSE("GPL");
