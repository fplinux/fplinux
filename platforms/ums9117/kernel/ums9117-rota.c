// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma/ums9117-dma.h>
#include <linux/dmaengine.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#define UMS9117_ROTA_NAME "ums9117-rota"

#define UMS9117_ROTA_PHYS 0x20900000ULL
#define UMS9117_ROTA_MMIO_BYTES 0x450U
#define UMS9117_ROTA_LIST_OFFSET 0x420U
#define UMS9117_ROTA_LIST_BYTES 0x30U

#define UMS9117_AP_AHB_GATE_STATE_PHYS 0x20e00000ULL
#define UMS9117_AP_AHB_GATE_SET_PHYS 0x20e01000ULL
#define UMS9117_AP_AHB_GATE_CLEAR_PHYS 0x20e02000ULL
#define UMS9117_AP_AHB_RESET_SET_PHYS 0x20e01004ULL
#define UMS9117_AP_AHB_RESET_CLEAR_PHYS 0x20e02004ULL

#define UMS9117_AP_AHB_ROTA_GATE BIT(13)
#define UMS9117_AP_AHB_ROTA_RESET BIT(3)

#define UMS9117_ROTA_SOURCE 0x400U
#define UMS9117_ROTA_DESTINATION 0x404U
#define UMS9117_ROTA_IMAGE_SIZE 0x408U
#define UMS9117_ROTA_CONTROL 0x40cU
#define UMS9117_ROTA_ORIGINAL_WIDTH 0x410U
#define UMS9117_ROTA_ORIGINAL_OFFSET 0x414U

#define UMS9117_ROTA_IMAGE_HEIGHT GENMASK(11, 0)
#define UMS9117_ROTA_IMAGE_WIDTH GENMASK(23, 12)
#define UMS9117_ROTA_IMAGE_SAMPLE GENMASK(25, 24)
#define UMS9117_ROTA_CONTROL_SPECIAL_422 BIT(0)
#define UMS9117_ROTA_CONTROL_DIRECTION GENMASK(2, 1)
#define UMS9117_ROTA_CONTROL_ENABLE BIT(3)

#define UMS9117_ROTA_MAX_DIMENSION 4092U
#define UMS9117_ROTA_MAX_PITCH 4095U
#define UMS9117_ROTA_TIMEOUT_MS 5000U

enum ums9117_rota_direction {
	UMS9117_ROTA_90 = 0,
	UMS9117_ROTA_270 = 1,
	UMS9117_ROTA_180 = 2,
	UMS9117_ROTA_MIRROR = 3,
};

struct ums9117_rota_format {
	u32 fourcc;
	u8 num_planes;
	u8 sample_bytes;
	u8 vertical_subsampling;
	bool yuv;
};

static const struct ums9117_rota_format ums9117_rota_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_RGB565,
		.num_planes = 1,
		.sample_bytes = 2,
		.vertical_subsampling = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_XRGB32,
		.num_planes = 1,
		.sample_bytes = 4,
		.vertical_subsampling = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_GREY,
		.num_planes = 1,
		.sample_bytes = 1,
		.vertical_subsampling = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12M,
		.num_planes = 2,
		.sample_bytes = 1,
		.vertical_subsampling = 2,
		.yuv = true,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV16M,
		.num_planes = 2,
		.sample_bytes = 1,
		.vertical_subsampling = 1,
		.yuv = true,
	},
};

struct ums9117_rota_frame {
	const struct ums9117_rota_format *format;
	struct v4l2_pix_format_mplane pix;
	struct v4l2_rect crop;
};

struct ums9117_rota_dev;

struct ums9117_rota_ctx {
	struct v4l2_fh fh;
	struct ums9117_rota_dev *rota;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *operation_controls[2];
	struct ums9117_rota_frame source;
	struct ums9117_rota_frame destination;
	u32 rotation;
	bool hflip;
	u8 pass;
	u32 output_sequence;
	u32 capture_sequence;
	struct dma_buf *pins[2 * VIDEO_MAX_PLANES];
	unsigned int num_pins;
};

struct ums9117_rota_dev {
	struct device *dev;
	struct v4l2_device v4l2;
	struct video_device video;
	struct v4l2_m2m_dev *m2m;
	struct mutex device_lock;
	struct mutex hardware_lock;
	spinlock_t callback_lock;
	struct ums9117_rota_ctx *active_ctx;
	struct dmaengine_result completion_result;
	bool completion_pending;
	bool stopping;
	bool fatal;
	bool gate_acquired;
	void __iomem *base;
	void __iomem *gate_state;
	void __iomem *gate_set;
	void __iomem *gate_clear;
	void __iomem *reset_set;
	void __iomem *reset_clear;
	struct dma_chan *dma;
	struct work_struct completion_work;
	struct delayed_work timeout_work;
	unsigned long deadline;
};

static const struct ums9117_rota_format *ums9117_rota_find_format(u32 fourcc)
{
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(ums9117_rota_formats); ++index)
		if (ums9117_rota_formats[index].fourcc == fourcc)
			return &ums9117_rota_formats[index];
	return NULL;
}

static struct ums9117_rota_ctx *ums9117_rota_file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct ums9117_rota_ctx, fh);
}

static bool ums9117_rota_quarter_turn(const struct ums9117_rota_ctx *ctx)
{
	return !ctx->hflip && (ctx->rotation == 90 || ctx->rotation == 270);
}

static int ums9117_rota_direction(const struct ums9117_rota_ctx *ctx,
				  enum ums9117_rota_direction *direction)
{
	if (ctx->hflip && ctx->rotation == 0) {
		*direction = UMS9117_ROTA_MIRROR;
		return 0;
	}
	if (ctx->hflip)
		return -EINVAL;

	switch (ctx->rotation) {
	case 90:
		*direction = UMS9117_ROTA_90;
		return 0;
	case 180:
		*direction = UMS9117_ROTA_180;
		return 0;
	case 270:
		*direction = UMS9117_ROTA_270;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ums9117_rota_plane_size(u32 bytesperline, u32 height, u32 *size)
{
	if (check_mul_overflow(bytesperline, height, size))
		return -EINVAL;
	return 0;
}

static int
ums9117_rota_prepare_source_format(struct v4l2_pix_format_mplane *pix)
{
	const struct ums9117_rota_format *format;
	u32 minimum_pitch;
	u32 maximum_pitch;
	u32 height;
	u32 pitch;
	int ret;

	format = ums9117_rota_find_format(pix->pixelformat);
	if (!format)
		format = &ums9117_rota_formats[0];

	pix->width = clamp_t(u32, pix->width, format->yuv ? 2U : 1U,
			     UMS9117_ROTA_MAX_DIMENSION);
	pix->height = clamp_t(u32, pix->height, format->vertical_subsampling,
			      UMS9117_ROTA_MAX_DIMENSION);
	if (format->yuv)
		pix->width &= ~1U;
	pix->height -= pix->height % format->vertical_subsampling;
	pix->pixelformat = format->fourcc;
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = format->num_planes;
	pix->flags = 0;

	minimum_pitch = pix->width * format->sample_bytes;
	maximum_pitch = UMS9117_ROTA_MAX_PITCH * format->sample_bytes;
	pitch = clamp_t(u32, pix->plane_fmt[0].bytesperline, minimum_pitch,
			maximum_pitch);
	pitch = ALIGN(pitch, 4);
	if (pitch > maximum_pitch)
		pitch = maximum_pitch & ~3U;
	pix->plane_fmt[0].bytesperline = pitch;
	ret = ums9117_rota_plane_size(pitch, pix->height,
				      &pix->plane_fmt[0].sizeimage);
	if (ret)
		return ret;

	if (format->num_planes == 2) {
		minimum_pitch = pix->width;
		maximum_pitch = UMS9117_ROTA_MAX_PITCH * 2U;
		pitch = clamp_t(u32, pix->plane_fmt[1].bytesperline,
				minimum_pitch, maximum_pitch);
		pitch = ALIGN(pitch, 4);
		if (pitch > maximum_pitch)
			pitch = maximum_pitch & ~3U;
		pix->plane_fmt[1].bytesperline = pitch;
		height = pix->height / format->vertical_subsampling;
		ret = ums9117_rota_plane_size(pitch, height,
					      &pix->plane_fmt[1].sizeimage);
		if (ret)
			return ret;
	}
	return 0;
}

static void
ums9117_rota_copy_colorimetry(struct v4l2_pix_format_mplane *destination,
			      const struct v4l2_pix_format_mplane *source)
{
	destination->colorspace = source->colorspace;
	destination->xfer_func = source->xfer_func;
	destination->ycbcr_enc = source->ycbcr_enc;
	destination->quantization = source->quantization;
}

static int ums9117_rota_derive_destination(struct ums9117_rota_ctx *ctx,
					   struct v4l2_pix_format_mplane *pix)
{
	const struct ums9117_rota_format *format = ctx->source.format;
	u32 height;
	unsigned int plane;
	int ret;

	if (ums9117_rota_quarter_turn(ctx)) {
		pix->width = ctx->source.crop.height;
		pix->height = ctx->source.crop.width;
	} else {
		pix->width = ctx->source.crop.width;
		pix->height = ctx->source.crop.height;
	}
	pix->pixelformat = format->fourcc;
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = format->num_planes;
	pix->flags = 0;
	for (plane = 0; plane < format->num_planes; ++plane) {
		pix->plane_fmt[plane].bytesperline =
			pix->width * (plane ? 1U : format->sample_bytes);
		height = pix->height;
		if (plane)
			height /= format->vertical_subsampling;
		ret = ums9117_rota_plane_size(
			pix->plane_fmt[plane].bytesperline, height,
			&pix->plane_fmt[plane].sizeimage);
		if (ret)
			return ret;
	}
	ums9117_rota_copy_colorimetry(pix, &ctx->source.pix);
	return 0;
}

static void ums9117_rota_set_default_frames(struct ums9117_rota_ctx *ctx)
{
	struct v4l2_pix_format_mplane *pix = &ctx->source.pix;

	memset(pix, 0, sizeof(*pix));
	pix->width = 240;
	pix->height = 320;
	pix->pixelformat = V4L2_PIX_FMT_RGB565;
	ums9117_rota_prepare_source_format(pix);
	ctx->source.format = ums9117_rota_find_format(pix->pixelformat);
	ctx->source.crop.left = 0;
	ctx->source.crop.top = 0;
	ctx->source.crop.width = pix->width;
	ctx->source.crop.height = pix->height;
	ctx->rotation = 90;
	ctx->hflip = false;
	memset(&ctx->destination.pix, 0, sizeof(ctx->destination.pix));
	ctx->destination.format = ctx->source.format;
	ums9117_rota_derive_destination(ctx, &ctx->destination.pix);
	ctx->destination.crop.left = 0;
	ctx->destination.crop.top = 0;
	ctx->destination.crop.width = ctx->destination.pix.width;
	ctx->destination.crop.height = ctx->destination.pix.height;
}

static struct ums9117_rota_frame *
ums9117_rota_get_frame(struct ums9117_rota_ctx *ctx, enum v4l2_buf_type type)
{
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return &ctx->source;
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return &ctx->destination;
	return NULL;
}

static int ums9117_rota_querycap(struct file *file, void *priv,
				 struct v4l2_capability *cap)
{
	strscpy(cap->driver, UMS9117_ROTA_NAME, sizeof(cap->driver));
	strscpy(cap->card, "UMS9117 image rotation", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" UMS9117_ROTA_NAME,
		sizeof(cap->bus_info));
	return 0;
}

static int ums9117_rota_enum_format(struct file *file, void *priv,
				    struct v4l2_fmtdesc *description)
{
	if (description->index >= ARRAY_SIZE(ums9117_rota_formats))
		return -EINVAL;
	description->pixelformat =
		ums9117_rota_formats[description->index].fourcc;
	return 0;
}

static int ums9117_rota_enum_framesizes(struct file *file, void *priv,
					struct v4l2_frmsizeenum *size)
{
	const struct ums9117_rota_format *format;

	if (size->index)
		return -EINVAL;
	format = ums9117_rota_find_format(size->pixel_format);
	if (!format)
		return -EINVAL;
	size->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	size->stepwise.min_width = format->yuv ? 2 : 1;
	size->stepwise.min_height = format->vertical_subsampling;
	size->stepwise.max_width = UMS9117_ROTA_MAX_DIMENSION;
	size->stepwise.max_height = UMS9117_ROTA_MAX_DIMENSION;
	size->stepwise.step_width = format->yuv ? 2 : 1;
	size->stepwise.step_height = format->vertical_subsampling;
	return 0;
}

static int ums9117_rota_g_format(struct file *file, void *priv,
				 struct v4l2_format *format)
{
	struct ums9117_rota_ctx *ctx = ums9117_rota_file_to_ctx(file);
	struct ums9117_rota_frame *frame;

	frame = ums9117_rota_get_frame(ctx, format->type);
	if (!frame)
		return -EINVAL;
	format->fmt.pix_mp = frame->pix;
	return 0;
}

static int ums9117_rota_try_format(struct file *file, void *priv,
				   struct v4l2_format *format)
{
	struct ums9117_rota_ctx *ctx = ums9117_rota_file_to_ctx(file);

	if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return ums9117_rota_prepare_source_format(&format->fmt.pix_mp);
	if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;
	return ums9117_rota_derive_destination(ctx, &format->fmt.pix_mp);
}

static bool ums9117_rota_queues_busy(struct ums9117_rota_ctx *ctx)
{
	struct vb2_queue *capture;
	struct vb2_queue *output;

	output = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
				 V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	capture = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
				  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	return vb2_is_busy(output) || vb2_is_busy(capture);
}

static void ums9117_rota_reset_destination(struct ums9117_rota_ctx *ctx)
{
	ctx->destination.format = ctx->source.format;
	ums9117_rota_derive_destination(ctx, &ctx->destination.pix);
	ctx->destination.crop.left = 0;
	ctx->destination.crop.top = 0;
	ctx->destination.crop.width = ctx->destination.pix.width;
	ctx->destination.crop.height = ctx->destination.pix.height;
}

static int ums9117_rota_s_format(struct file *file, void *priv,
				 struct v4l2_format *format)
{
	struct ums9117_rota_ctx *ctx = ums9117_rota_file_to_ctx(file);
	struct vb2_queue *queue;
	int ret;

	ret = ums9117_rota_try_format(file, priv, format);
	if (ret)
		return ret;
	queue = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, format->type);
	if (!queue || vb2_is_busy(queue))
		return -EBUSY;

	if (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		ctx->destination.pix = format->fmt.pix_mp;
		return 0;
	}
	if (ums9117_rota_queues_busy(ctx))
		return -EBUSY;
	ctx->source.pix = format->fmt.pix_mp;
	ctx->source.format =
		ums9117_rota_find_format(ctx->source.pix.pixelformat);
	ctx->source.crop.left = 0;
	ctx->source.crop.top = 0;
	ctx->source.crop.width = ctx->source.pix.width;
	ctx->source.crop.height = ctx->source.pix.height;
	ums9117_rota_reset_destination(ctx);
	return 0;
}

static int ums9117_rota_g_selection(struct file *file, void *priv,
				    struct v4l2_selection *selection)
{
	struct ums9117_rota_ctx *ctx = ums9117_rota_file_to_ctx(file);

	if (selection->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;
	switch (selection->target) {
	case V4L2_SEL_TGT_CROP:
		selection->r = ctx->source.crop;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		selection->r.left = 0;
		selection->r.top = 0;
		selection->r.width = ctx->source.pix.width;
		selection->r.height = ctx->source.pix.height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ums9117_rota_validate_crop(struct ums9117_rota_ctx *ctx,
				      const struct v4l2_rect *crop)
{
	u32 right;
	u32 bottom;

	if (crop->left < 0 || crop->top < 0 || crop->width <= 0 ||
	    crop->height <= 0 ||
	    check_add_overflow((u32)crop->left, (u32)crop->width, &right) ||
	    check_add_overflow((u32)crop->top, (u32)crop->height, &bottom) ||
	    right > ctx->source.pix.width || bottom > ctx->source.pix.height)
		return -EINVAL;
	if (ctx->source.format->yuv && ((crop->left | crop->width) & 1))
		return -EINVAL;
	if (ctx->source.format->vertical_subsampling > 1 &&
	    ((crop->top | crop->height) & 1))
		return -EINVAL;
	return 0;
}

static int ums9117_rota_s_selection(struct file *file, void *priv,
				    struct v4l2_selection *selection)
{
	struct ums9117_rota_ctx *ctx = ums9117_rota_file_to_ctx(file);
	int ret;

	if (selection->type != V4L2_BUF_TYPE_VIDEO_OUTPUT ||
	    selection->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;
	ret = ums9117_rota_validate_crop(ctx, &selection->r);
	if (ret)
		return ret;
	if (ums9117_rota_queues_busy(ctx))
		return -EBUSY;
	ctx->source.crop = selection->r;
	ums9117_rota_reset_destination(ctx);
	return 0;
}

static int ums9117_rota_try_control(struct v4l2_ctrl *control)
{
	struct ums9117_rota_ctx *ctx = container_of(
		control->handler, struct ums9117_rota_ctx, controls);
	u32 rotation = ctx->operation_controls[0]->val;
	bool hflip = ctx->operation_controls[1]->val;

	if (hflip)
		return rotation == 0 ? 0 : -EINVAL;
	return rotation == 90 || rotation == 180 || rotation == 270 ? 0 :
								      -EINVAL;
}

static int ums9117_rota_s_control(struct v4l2_ctrl *control)
{
	struct ums9117_rota_ctx *ctx = container_of(
		control->handler, struct ums9117_rota_ctx, controls);
	u32 rotation = ctx->operation_controls[0]->val;
	bool hflip = ctx->operation_controls[1]->val;

	/* Global controls must not change parameters of already queued frames. */
	if (ums9117_rota_queues_busy(ctx))
		return -EBUSY;
	ctx->rotation = rotation;
	ctx->hflip = hflip;
	ums9117_rota_reset_destination(ctx);
	return 0;
}

static const struct v4l2_ctrl_ops ums9117_rota_control_ops = {
	.try_ctrl = ums9117_rota_try_control,
	.s_ctrl = ums9117_rota_s_control,
};

static int ums9117_rota_setup_controls(struct ums9117_rota_ctx *ctx)
{
	v4l2_ctrl_handler_init(&ctx->controls, 2);
	ctx->operation_controls[0] =
		v4l2_ctrl_new_std(&ctx->controls, &ums9117_rota_control_ops,
				  V4L2_CID_ROTATE, 0, 270, 90, 90);
	ctx->operation_controls[1] =
		v4l2_ctrl_new_std(&ctx->controls, &ums9117_rota_control_ops,
				  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (ctx->controls.error)
		return ctx->controls.error;
	v4l2_ctrl_cluster(ARRAY_SIZE(ctx->operation_controls),
			  ctx->operation_controls);
	return v4l2_ctrl_handler_setup(&ctx->controls);
}

static int ums9117_rota_queue_setup(struct vb2_queue *queue,
				    unsigned int *num_buffers,
				    unsigned int *num_planes,
				    unsigned int sizes[],
				    struct device *alloc_devs[])
{
	struct ums9117_rota_ctx *ctx = vb2_get_drv_priv(queue);
	struct ums9117_rota_frame *frame =
		ums9117_rota_get_frame(ctx, queue->type);
	unsigned int plane;

	if (!frame)
		return -EINVAL;
	if (*num_planes) {
		if (*num_planes != frame->pix.num_planes)
			return -EINVAL;
		for (plane = 0; plane < *num_planes; ++plane)
			if (sizes[plane] <
			    frame->pix.plane_fmt[plane].sizeimage)
				return -EINVAL;
		return 0;
	}
	*num_planes = frame->pix.num_planes;
	for (plane = 0; plane < *num_planes; ++plane)
		sizes[plane] = frame->pix.plane_fmt[plane].sizeimage;
	return 0;
}

static int ums9117_rota_buffer_prepare(struct vb2_buffer *buffer)
{
	struct ums9117_rota_ctx *ctx = vb2_get_drv_priv(buffer->vb2_queue);
	struct ums9117_rota_frame *frame =
		ums9117_rota_get_frame(ctx, buffer->vb2_queue->type);
	bool output = V4L2_TYPE_IS_OUTPUT(buffer->vb2_queue->type);
	unsigned int plane;

	if (!frame || buffer->num_planes != frame->pix.num_planes)
		return -EINVAL;
	for (plane = 0; plane < buffer->num_planes; ++plane) {
		u32 size = frame->pix.plane_fmt[plane].sizeimage;

		if (buffer->planes[plane].data_offset ||
		    vb2_plane_size(buffer, plane) < size)
			return -EINVAL;
		if (output) {
			if (vb2_get_plane_payload(buffer, plane) < size)
				return -EINVAL;
		} else {
			vb2_set_plane_payload(buffer, plane, size);
		}
	}
	return 0;
}

static void ums9117_rota_buffer_queue(struct vb2_buffer *buffer)
{
	struct ums9117_rota_ctx *ctx = vb2_get_drv_priv(buffer->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(buffer));
}

static void ums9117_rota_return_queued(struct vb2_queue *queue,
				       enum vb2_buffer_state state)
{
	struct ums9117_rota_ctx *ctx = vb2_get_drv_priv(queue);
	struct vb2_v4l2_buffer *buffer;

	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(queue->type))
			buffer = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			buffer = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!buffer)
			break;
		v4l2_m2m_buf_done(buffer, state);
	}
}

static int ums9117_rota_validate_geometry(struct ums9117_rota_ctx *ctx)
{
	const struct v4l2_rect *crop = &ctx->source.crop;
	const struct ums9117_rota_format *format = ctx->source.format;
	u32 offset;
	unsigned int plane;

	for (plane = 0; plane < format->num_planes; plane++) {
		if (plane) {
			offset = crop->top / format->vertical_subsampling *
					 ctx->source.pix.plane_fmt[plane]
						 .bytesperline +
				 crop->left;
		} else {
			offset = crop->top * ctx->source.pix.plane_fmt[plane]
						     .bytesperline +
				 crop->left * format->sample_bytes;
		}
		if (!IS_ALIGNED(offset, 4))
			return -EINVAL;
	}

	for (plane = 0; plane < format->num_planes; plane++)
		if (!IS_ALIGNED(
			    ctx->destination.pix.plane_fmt[plane].bytesperline,
			    4))
			return -EINVAL;
	return 0;
}

static int ums9117_rota_start_streaming(struct vb2_queue *queue,
					unsigned int count)
{
	struct ums9117_rota_ctx *ctx = vb2_get_drv_priv(queue);
	int ret;

	if (READ_ONCE(ctx->rota->fatal)) {
		ums9117_rota_return_queued(queue, VB2_BUF_STATE_QUEUED);
		return -EIO;
	}
	ret = ums9117_rota_validate_geometry(ctx);
	if (ret) {
		ums9117_rota_return_queued(queue, VB2_BUF_STATE_QUEUED);
		return ret;
	}
	if (V4L2_TYPE_IS_OUTPUT(queue->type))
		ctx->output_sequence = 0;
	else
		ctx->capture_sequence = 0;
	return 0;
}

static void ums9117_rota_stop_streaming(struct vb2_queue *queue)
{
	ums9117_rota_return_queued(queue, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops ums9117_rota_queue_ops = {
	.queue_setup = ums9117_rota_queue_setup,
	.buf_prepare = ums9117_rota_buffer_prepare,
	.buf_queue = ums9117_rota_buffer_queue,
	.start_streaming = ums9117_rota_start_streaming,
	.stop_streaming = ums9117_rota_stop_streaming,
};

static int ums9117_rota_queue_init(void *priv, struct vb2_queue *source,
				   struct vb2_queue *destination)
{
	struct ums9117_rota_ctx *ctx = priv;
	int ret;

	source->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	source->io_modes = VB2_MMAP | VB2_DMABUF;
	source->drv_priv = ctx;
	source->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	source->min_queued_buffers = 1;
	source->ops = &ums9117_rota_queue_ops;
	source->mem_ops = &vb2_dma_contig_memops;
	source->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	source->lock = &ctx->rota->device_lock;
	source->dev = ctx->rota->dev;
	source->gfp_flags = __GFP_DMA32;
	ret = vb2_queue_init(source);
	if (ret)
		return ret;

	destination->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	destination->io_modes = VB2_MMAP | VB2_DMABUF;
	destination->drv_priv = ctx;
	destination->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	destination->min_queued_buffers = 1;
	destination->ops = &ums9117_rota_queue_ops;
	destination->mem_ops = &vb2_dma_contig_memops;
	destination->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	destination->lock = &ctx->rota->device_lock;
	destination->dev = ctx->rota->dev;
	destination->gfp_flags = __GFP_DMA32;
	return vb2_queue_init(destination);
}

static int ums9117_rota_pin_plane(struct ums9117_rota_ctx *ctx,
				  struct vb2_buffer *buffer, unsigned int plane)
{
	struct dma_buf *dma_buf;

	if (buffer->planes[plane].dbuf) {
		dma_buf = buffer->planes[plane].dbuf;
		get_dma_buf(dma_buf);
	} else {
		dma_buf = buffer->vb2_queue->mem_ops->get_dmabuf(
			buffer, buffer->planes[plane].mem_priv, O_RDWR);
		if (!dma_buf)
			return -ENOMEM;
	}
	ctx->pins[ctx->num_pins++] = dma_buf;
	return 0;
}

static void ums9117_rota_release_pins(struct ums9117_rota_ctx *ctx)
{
	while (ctx->num_pins)
		dma_buf_put(ctx->pins[--ctx->num_pins]);
}

static int ums9117_rota_pin_buffers(struct ums9117_rota_ctx *ctx,
				    struct vb2_v4l2_buffer *source,
				    struct vb2_v4l2_buffer *destination)
{
	unsigned int plane;
	int ret;

	for (plane = 0; plane < source->vb2_buf.num_planes; ++plane) {
		ret = ums9117_rota_pin_plane(ctx, &source->vb2_buf, plane);
		if (ret)
			goto error;
	}
	for (plane = 0; plane < destination->vb2_buf.num_planes; ++plane) {
		ret = ums9117_rota_pin_plane(ctx, &destination->vb2_buf, plane);
		if (ret)
			goto error;
	}
	return 0;

error:
	ums9117_rota_release_pins(ctx);
	return ret;
}

static bool ums9117_rota_ranges_overlap(dma_addr_t first, u32 first_size,
					dma_addr_t second, u32 second_size)
{
	return first < second + second_size && second < first + first_size;
}

static int ums9117_rota_validate_addresses(struct ums9117_rota_ctx *ctx,
					   struct vb2_v4l2_buffer *source,
					   struct vb2_v4l2_buffer *destination)
{
	dma_addr_t source_address[VIDEO_MAX_PLANES];
	dma_addr_t destination_address[VIDEO_MAX_PLANES];
	u32 source_size[VIDEO_MAX_PLANES];
	u32 destination_size[VIDEO_MAX_PLANES];
	unsigned int source_plane;
	unsigned int destination_plane;

	for (source_plane = 0; source_plane < source->vb2_buf.num_planes;
	     ++source_plane) {
		source_address[source_plane] = vb2_dma_contig_plane_dma_addr(
			&source->vb2_buf, source_plane);
		source_size[source_plane] =
			ctx->source.pix.plane_fmt[source_plane].sizeimage;
		if (!IS_ALIGNED(source_address[source_plane], 4) ||
		    upper_32_bits(source_address[source_plane]) ||
		    source_size[source_plane] - 1 >
			    U32_MAX - source_address[source_plane])
			return -EINVAL;
	}
	for (destination_plane = 0;
	     destination_plane < destination->vb2_buf.num_planes;
	     ++destination_plane) {
		destination_address[destination_plane] =
			vb2_dma_contig_plane_dma_addr(&destination->vb2_buf,
						      destination_plane);
		destination_size[destination_plane] =
			ctx->destination.pix.plane_fmt[destination_plane]
				.sizeimage;
		if (!IS_ALIGNED(destination_address[destination_plane], 4) ||
		    upper_32_bits(destination_address[destination_plane]) ||
		    destination_size[destination_plane] - 1 >
			    U32_MAX - destination_address[destination_plane])
			return -EINVAL;
	}
	for (source_plane = 0; source_plane < source->vb2_buf.num_planes;
	     ++source_plane)
		for (destination_plane = 0;
		     destination_plane < destination->vb2_buf.num_planes;
		     ++destination_plane)
			if (ums9117_rota_ranges_overlap(
				    source_address[source_plane],
				    source_size[source_plane],
				    destination_address[destination_plane],
				    destination_size[destination_plane]))
				return -EINVAL;
	return 0;
}

static void ums9117_rota_reset(struct ums9117_rota_dev *rota)
{
	writel(UMS9117_AP_AHB_ROTA_RESET, rota->reset_set);
	udelay(1);
	writel(UMS9117_AP_AHB_ROTA_RESET, rota->reset_clear);
	readl(rota->base + UMS9117_ROTA_CONTROL);
}

static void ums9117_rota_dma_callback(void *parameter,
				      const struct dmaengine_result *result)
{
	struct ums9117_rota_dev *rota = parameter;
	unsigned long flags;

	spin_lock_irqsave(&rota->callback_lock, flags);
	if (!rota->active_ctx || rota->stopping || rota->completion_pending) {
		spin_unlock_irqrestore(&rota->callback_lock, flags);
		return;
	}
	if (result)
		rota->completion_result = *result;
	else
		rota->completion_result.result = DMA_TRANS_ABORTED;
	rota->completion_pending = true;
	spin_unlock_irqrestore(&rota->callback_lock, flags);
	schedule_work(&rota->completion_work);
}

static int ums9117_rota_program_pass(struct ums9117_rota_ctx *ctx)
{
	struct ums9117_rota_dev *rota = ctx->rota;
	struct vb2_v4l2_buffer *source;
	struct vb2_v4l2_buffer *destination;
	struct dma_async_tx_descriptor *descriptor;
	enum ums9117_rota_direction direction;
	dma_cookie_t cookie;
	dma_addr_t source_address;
	dma_addr_t destination_address;
	u32 width = ctx->source.crop.width;
	u32 height = ctx->source.crop.height;
	u32 original_width;
	u32 offset_x = ctx->source.crop.left;
	u32 offset_y = ctx->source.crop.top;
	u32 sample_bytes = ctx->source.format->sample_bytes;
	u32 sample_code;
	u32 control;
	u32 image_size;
	bool special_422 = false;
	int ret;

	ret = ums9117_rota_direction(ctx, &direction);
	if (ret)
		return ret;
	source = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	destination = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!source || !destination)
		return -EINVAL;

	source_address =
		vb2_dma_contig_plane_dma_addr(&source->vb2_buf, ctx->pass);
	destination_address =
		vb2_dma_contig_plane_dma_addr(&destination->vb2_buf, ctx->pass);
	if (ctx->pass) {
		sample_bytes = 2;
		width /= 2;
		offset_x /= 2;
		original_width = ctx->source.pix.plane_fmt[1].bytesperline / 2;
		if (ctx->source.format->fourcc == V4L2_PIX_FMT_NV12M) {
			height /= 2;
			offset_y /= 2;
		} else if (ums9117_rota_quarter_turn(ctx)) {
			/* Anchor special-mode row parity at the selected crop. */
			source_address +=
				ctx->source.crop.top *
				ctx->source.pix.plane_fmt[1].bytesperline;
			height /= 2;
			offset_y = 0;
			special_422 = true;
		}
	} else {
		original_width = ctx->source.pix.plane_fmt[0].bytesperline /
				 ctx->source.format->sample_bytes;
	}

	sample_code = ilog2(sample_bytes);
	image_size = FIELD_PREP(UMS9117_ROTA_IMAGE_SAMPLE, sample_code) |
		     FIELD_PREP(UMS9117_ROTA_IMAGE_WIDTH, width) |
		     FIELD_PREP(UMS9117_ROTA_IMAGE_HEIGHT, height);
	control = FIELD_PREP(UMS9117_ROTA_CONTROL_DIRECTION, direction);
	if (special_422)
		control |= UMS9117_ROTA_CONTROL_SPECIAL_422;

	ums9117_rota_reset(rota);
	writel(lower_32_bits(source_address), rota->base + UMS9117_ROTA_SOURCE);
	writel(lower_32_bits(destination_address),
	       rota->base + UMS9117_ROTA_DESTINATION);
	writel(image_size, rota->base + UMS9117_ROTA_IMAGE_SIZE);
	writel(original_width, rota->base + UMS9117_ROTA_ORIGINAL_WIDTH);
	writel(offset_x | (offset_y << 12),
	       rota->base + UMS9117_ROTA_ORIGINAL_OFFSET);
	writel(control, rota->base + UMS9117_ROTA_CONTROL);

	descriptor = ums9117_dma_prep_rota(
		rota->dma, UMS9117_ROTA_PHYS + UMS9117_ROTA_LIST_OFFSET,
		UMS9117_ROTA_LIST_BYTES, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!descriptor)
		return -EIO;
	descriptor->callback_result = ums9117_rota_dma_callback;
	descriptor->callback_param = rota;
	cookie = dmaengine_submit(descriptor);
	if (dma_submit_error(cookie))
		return dma_submit_error(cookie);
	dma_async_issue_pending(rota->dma);
	/* Make the waiting DMA route visible before ROTA can request it. */
	wmb();
	writel(control | UMS9117_ROTA_CONTROL_ENABLE,
	       rota->base + UMS9117_ROTA_CONTROL);
	return 0;
}

static void ums9117_rota_retain_pins(struct ums9117_rota_ctx *ctx, int error)
{
	struct ums9117_rota_dev *rota = ctx->rota;

	if (!rota->fatal) {
		rota->fatal = true;
		if (THIS_MODULE)
			__module_get(THIS_MODULE);
		dev_err(rota->dev,
			"DMA did not stop (%d); active image memory retained until cold boot\n",
			error);
	}
	/* Deliberately leak the dma-buf references: hardware may still write. */
	ctx->num_pins = 0;
}

static void ums9117_rota_finish_job(struct ums9117_rota_ctx *ctx,
				    enum vb2_buffer_state state,
				    bool retain_pins)
{
	struct ums9117_rota_dev *rota = ctx->rota;
	struct vb2_v4l2_buffer *source;
	struct vb2_v4l2_buffer *destination;
	unsigned long flags;

	spin_lock_irqsave(&rota->callback_lock, flags);
	if (rota->active_ctx == ctx)
		rota->active_ctx = NULL;
	rota->completion_pending = false;
	rota->stopping = false;
	spin_unlock_irqrestore(&rota->callback_lock, flags);
	cancel_delayed_work(&rota->timeout_work);

	source = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	destination = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (source && destination) {
		v4l2_m2m_buf_copy_metadata(source, destination, true);
		source->sequence = ctx->output_sequence++;
		destination->sequence = ctx->capture_sequence++;
		v4l2_m2m_buf_done(source, state);
		v4l2_m2m_buf_done(destination, state);
	} else {
		if (source)
			v4l2_m2m_buf_done(source, VB2_BUF_STATE_ERROR);
		if (destination)
			v4l2_m2m_buf_done(destination, VB2_BUF_STATE_ERROR);
	}
	if (!retain_pins)
		ums9117_rota_release_pins(ctx);
	v4l2_m2m_job_finish(rota->m2m, ctx->fh.m2m_ctx);
}

static int ums9117_rota_stop_dma(struct ums9117_rota_ctx *ctx)
{
	int ret = dmaengine_terminate_sync(ctx->rota->dma);

	if (ret)
		ums9117_rota_retain_pins(ctx, ret);
	return ret;
}

static void ums9117_rota_completion_work(struct work_struct *work)
{
	struct ums9117_rota_dev *rota =
		container_of(work, struct ums9117_rota_dev, completion_work);
	struct ums9117_rota_ctx *ctx;
	struct dmaengine_result result;
	unsigned long flags;
	int ret;

	mutex_lock(&rota->hardware_lock);
	spin_lock_irqsave(&rota->callback_lock, flags);
	ctx = rota->active_ctx;
	if (!ctx || rota->stopping || !rota->completion_pending) {
		spin_unlock_irqrestore(&rota->callback_lock, flags);
		goto unlock;
	}
	result = rota->completion_result;
	rota->completion_pending = false;
	spin_unlock_irqrestore(&rota->callback_lock, flags);

	if (result.result != DMA_TRANS_NOERROR || result.residue) {
		ret = ums9117_rota_stop_dma(ctx);
		ums9117_rota_finish_job(ctx, VB2_BUF_STATE_ERROR, ret != 0);
		goto unlock;
	}
	if (!ctx->pass && ctx->source.format->num_planes == 2) {
		ctx->pass = 1;
		ret = ums9117_rota_program_pass(ctx);
		if (!ret)
			goto unlock;
		ret = ums9117_rota_stop_dma(ctx);
		ums9117_rota_finish_job(ctx, VB2_BUF_STATE_ERROR, ret != 0);
		goto unlock;
	}
	ums9117_rota_finish_job(ctx, VB2_BUF_STATE_DONE, false);

unlock:
	mutex_unlock(&rota->hardware_lock);
}

static void ums9117_rota_timeout_work(struct work_struct *work)
{
	struct ums9117_rota_dev *rota = container_of(
		to_delayed_work(work), struct ums9117_rota_dev, timeout_work);
	struct ums9117_rota_ctx *ctx;
	unsigned long flags;
	int ret;

	mutex_lock(&rota->hardware_lock);
	spin_lock_irqsave(&rota->callback_lock, flags);
	ctx = rota->active_ctx;
	if (!ctx || rota->stopping) {
		spin_unlock_irqrestore(&rota->callback_lock, flags);
		goto unlock;
	}
	if (time_before(jiffies, rota->deadline)) {
		spin_unlock_irqrestore(&rota->callback_lock, flags);
		mod_delayed_work(system_wq, &rota->timeout_work,
				 rota->deadline - jiffies);
		goto unlock;
	}
	rota->stopping = true;
	spin_unlock_irqrestore(&rota->callback_lock, flags);
	dev_err(rota->dev, "rotation timed out\n");
	ret = ums9117_rota_stop_dma(ctx);
	ums9117_rota_finish_job(ctx, VB2_BUF_STATE_ERROR, ret != 0);

unlock:
	mutex_unlock(&rota->hardware_lock);
}

static void ums9117_rota_device_run(void *priv)
{
	struct ums9117_rota_ctx *ctx = priv;
	struct ums9117_rota_dev *rota = ctx->rota;
	struct vb2_v4l2_buffer *source;
	struct vb2_v4l2_buffer *destination;
	unsigned long flags;
	int ret;

	mutex_lock(&rota->hardware_lock);
	source = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	destination = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!source || !destination || rota->fatal) {
		ums9117_rota_finish_job(ctx, VB2_BUF_STATE_ERROR, false);
		goto unlock;
	}
	ret = ums9117_rota_pin_buffers(ctx, source, destination);
	if (!ret)
		ret = ums9117_rota_validate_addresses(ctx, source, destination);
	if (ret) {
		ums9117_rota_finish_job(ctx, VB2_BUF_STATE_ERROR, false);
		goto unlock;
	}
	ctx->pass = 0;
	spin_lock_irqsave(&rota->callback_lock, flags);
	rota->active_ctx = ctx;
	rota->completion_pending = false;
	rota->stopping = false;
	spin_unlock_irqrestore(&rota->callback_lock, flags);
	ret = ums9117_rota_program_pass(ctx);
	if (ret) {
		ret = ums9117_rota_stop_dma(ctx);
		ums9117_rota_finish_job(ctx, VB2_BUF_STATE_ERROR, ret != 0);
		goto unlock;
	}
	rota->deadline = jiffies + msecs_to_jiffies(UMS9117_ROTA_TIMEOUT_MS);
	mod_delayed_work(system_wq, &rota->timeout_work,
			 msecs_to_jiffies(UMS9117_ROTA_TIMEOUT_MS));

unlock:
	mutex_unlock(&rota->hardware_lock);
}

static void ums9117_rota_job_abort(void *priv)
{
	struct ums9117_rota_ctx *ctx = priv;
	struct ums9117_rota_dev *rota = ctx->rota;
	unsigned long flags;
	bool is_current;
	int ret;

	mutex_lock(&rota->hardware_lock);
	spin_lock_irqsave(&rota->callback_lock, flags);
	is_current = rota->active_ctx == ctx;
	if (is_current)
		rota->stopping = true;
	spin_unlock_irqrestore(&rota->callback_lock, flags);
	if (is_current) {
		cancel_delayed_work(&rota->timeout_work);
		ret = ums9117_rota_stop_dma(ctx);
		ums9117_rota_finish_job(ctx, VB2_BUF_STATE_ERROR, ret != 0);
	}
	mutex_unlock(&rota->hardware_lock);
}

static const struct v4l2_m2m_ops ums9117_rota_m2m_ops = {
	.device_run = ums9117_rota_device_run,
	.job_abort = ums9117_rota_job_abort,
};

static int ums9117_rota_open(struct file *file)
{
	struct ums9117_rota_dev *rota = video_drvdata(file);
	struct ums9117_rota_ctx *ctx;
	int ret;

	if (mutex_lock_interruptible(&rota->device_lock))
		return -ERESTARTSYS;
	if (rota->fatal) {
		ret = -EIO;
		goto unlock;
	}
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto unlock;
	}
	ctx->rota = rota;
	ums9117_rota_set_default_frames(ctx);
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->fh.m2m_ctx =
		v4l2_m2m_ctx_init(rota->m2m, ctx, ums9117_rota_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto exit_fh;
	}
	ret = ums9117_rota_setup_controls(ctx);
	if (ret)
		goto release_m2m;
	ctx->fh.ctrl_handler = &ctx->controls;
	v4l2_fh_add(&ctx->fh, file);
	mutex_unlock(&rota->device_lock);
	return 0;

release_m2m:
	v4l2_ctrl_handler_free(&ctx->controls);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
exit_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
unlock:
	mutex_unlock(&rota->device_lock);
	return ret;
}

static int ums9117_rota_release(struct file *file)
{
	struct ums9117_rota_ctx *ctx = ums9117_rota_file_to_ctx(file);
	struct ums9117_rota_dev *rota = ctx->rota;

	mutex_lock(&rota->device_lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->controls);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&rota->device_lock);
	return 0;
}

static const struct v4l2_file_operations ums9117_rota_file_ops = {
	.owner = THIS_MODULE,
	.open = ums9117_rota_open,
	.release = ums9117_rota_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct v4l2_ioctl_ops ums9117_rota_ioctl_ops = {
	.vidioc_querycap = ums9117_rota_querycap,
	.vidioc_enum_framesizes = ums9117_rota_enum_framesizes,
	.vidioc_enum_fmt_vid_out = ums9117_rota_enum_format,
	.vidioc_g_fmt_vid_out_mplane = ums9117_rota_g_format,
	.vidioc_try_fmt_vid_out_mplane = ums9117_rota_try_format,
	.vidioc_s_fmt_vid_out_mplane = ums9117_rota_s_format,
	.vidioc_enum_fmt_vid_cap = ums9117_rota_enum_format,
	.vidioc_g_fmt_vid_cap_mplane = ums9117_rota_g_format,
	.vidioc_try_fmt_vid_cap_mplane = ums9117_rota_try_format,
	.vidioc_s_fmt_vid_cap_mplane = ums9117_rota_s_format,
	.vidioc_g_selection = ums9117_rota_g_selection,
	.vidioc_s_selection = ums9117_rota_s_selection,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_log_status = v4l2_ctrl_log_status,
};

static void __iomem *ums9117_rota_map_shared(struct platform_device *pdev,
					     const char *name,
					     resource_size_t address)
{
	struct resource *resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	void __iomem *base;

	if (!resource || resource->start != address ||
	    resource_size(resource) != 4)
		return IOMEM_ERR_PTR(-EINVAL);
	base = devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
	return base ? base : IOMEM_ERR_PTR(-ENOMEM);
}

static void ums9117_rota_restore_gate(struct ums9117_rota_dev *rota)
{
	if (!rota->gate_acquired)
		return;
	writel(UMS9117_AP_AHB_ROTA_GATE, rota->gate_clear);
	if (readl(rota->gate_state) & UMS9117_AP_AHB_ROTA_GATE)
		dev_err(rota->dev, "could not restore ROTA clock gate\n");
	else
		rota->gate_acquired = false;
}

static int ums9117_rota_probe(struct platform_device *pdev)
{
	struct ums9117_rota_dev *rota;
	struct resource *resource;
	u32 gate_state;
	int ret;

	rota = devm_kzalloc(&pdev->dev, sizeof(*rota), GFP_KERNEL);
	if (!rota)
		return -ENOMEM;
	rota->dev = &pdev->dev;
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rota");
	if (!resource || resource->start != UMS9117_ROTA_PHYS ||
	    resource_size(resource) != UMS9117_ROTA_MMIO_BYTES)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid ROTA register resource\n");
	rota->base = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(rota->base))
		return PTR_ERR(rota->base);
	rota->gate_state = ums9117_rota_map_shared(
		pdev, "ap-ahb-gate-state", UMS9117_AP_AHB_GATE_STATE_PHYS);
	rota->gate_set = ums9117_rota_map_shared(pdev, "ap-ahb-gate-set",
						 UMS9117_AP_AHB_GATE_SET_PHYS);
	rota->gate_clear = ums9117_rota_map_shared(
		pdev, "ap-ahb-gate-clear", UMS9117_AP_AHB_GATE_CLEAR_PHYS);
	rota->reset_set = ums9117_rota_map_shared(
		pdev, "ap-ahb-reset-set", UMS9117_AP_AHB_RESET_SET_PHYS);
	rota->reset_clear = ums9117_rota_map_shared(
		pdev, "ap-ahb-reset-clear", UMS9117_AP_AHB_RESET_CLEAR_PHYS);
	if (IS_ERR(rota->gate_state) || IS_ERR(rota->gate_set) ||
	    IS_ERR(rota->gate_clear) || IS_ERR(rota->reset_set) ||
	    IS_ERR(rota->reset_clear))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid AP AHB resources\n");
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	rota->dma = ums9117_dma_request_rota();
	if (IS_ERR(rota->dma))
		return dev_err_probe(&pdev->dev, PTR_ERR(rota->dma),
				     "could not reserve ROTA DMA route\n");
	gate_state = readl(rota->gate_state);
	rota->gate_acquired = !(gate_state & UMS9117_AP_AHB_ROTA_GATE);
	if (rota->gate_acquired)
		writel(UMS9117_AP_AHB_ROTA_GATE, rota->gate_set);
	if (!(readl(rota->gate_state) & UMS9117_AP_AHB_ROTA_GATE)) {
		ret = -EIO;
		goto release_dma;
	}

	mutex_init(&rota->device_lock);
	mutex_init(&rota->hardware_lock);
	spin_lock_init(&rota->callback_lock);
	INIT_WORK(&rota->completion_work, ums9117_rota_completion_work);
	INIT_DELAYED_WORK(&rota->timeout_work, ums9117_rota_timeout_work);
	ret = v4l2_device_register(&pdev->dev, &rota->v4l2);
	if (ret)
		goto restore_gate;
	rota->m2m = v4l2_m2m_init(&ums9117_rota_m2m_ops);
	if (IS_ERR(rota->m2m)) {
		ret = PTR_ERR(rota->m2m);
		goto unregister_v4l2;
	}
	rota->video.fops = &ums9117_rota_file_ops;
	rota->video.ioctl_ops = &ums9117_rota_ioctl_ops;
	rota->video.v4l2_dev = &rota->v4l2;
	rota->video.lock = &rota->device_lock;
	rota->video.release = video_device_release_empty;
	rota->video.vfl_dir = VFL_DIR_M2M;
	rota->video.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE |
				  V4L2_CAP_STREAMING;
	strscpy(rota->video.name, UMS9117_ROTA_NAME, sizeof(rota->video.name));
	video_set_drvdata(&rota->video, rota);
	ret = video_register_device(&rota->video, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto release_m2m;
	platform_set_drvdata(pdev, rota);
	dev_info(&pdev->dev, "registered as /dev/video%d\n", rota->video.num);
	return 0;

release_m2m:
	v4l2_m2m_release(rota->m2m);
unregister_v4l2:
	v4l2_device_unregister(&rota->v4l2);
restore_gate:
	ums9117_rota_restore_gate(rota);
release_dma:
	dma_release_channel(rota->dma);
	return dev_err_probe(&pdev->dev, ret, "could not initialize ROTA\n");
}

static void ums9117_rota_remove(struct platform_device *pdev)
{
	struct ums9117_rota_dev *rota = platform_get_drvdata(pdev);

	video_unregister_device(&rota->video);
	cancel_delayed_work_sync(&rota->timeout_work);
	cancel_work_sync(&rota->completion_work);
	v4l2_m2m_release(rota->m2m);
	v4l2_device_unregister(&rota->v4l2);
	dmaengine_terminate_sync(rota->dma);
	dma_release_channel(rota->dma);
	ums9117_rota_restore_gate(rota);
}

static const struct of_device_id ums9117_rota_of_match[] = {
	{ .compatible = "sprd,ums9117-rota" },
	{},
};
MODULE_DEVICE_TABLE(of, ums9117_rota_of_match);

static struct platform_driver ums9117_rota_driver = {
	.probe = ums9117_rota_probe,
	.remove = ums9117_rota_remove,
	.driver = {
		.name = UMS9117_ROTA_NAME,
		.of_match_table = ums9117_rota_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ums9117_rota_driver);

MODULE_DESCRIPTION("Unisoc UMS9117 V4L2 image rotation driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
