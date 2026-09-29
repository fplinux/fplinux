// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <linux/media-bus-format.h>
#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "ums9117-dcam-capture.h"
#include "ums9117-jpeg-hw.h"

#define DCAM_NAME "ums9117-dcam"
#define DCAM_PREVIEW_WIDTH 800U
#define DCAM_PREVIEW_HEIGHT 600U
#define DCAM_STILL_WIDTH 1600U
#define DCAM_STILL_HEIGHT 1200U
#define DCAM_SERIAL_WIDTH 240U
#define DCAM_SERIAL_HEIGHT 320U
#define DCAM_SERIAL_FRAME_TIMEOUT_MS 3000U
#define DCAM_PREVIEW_STARTUP_SKIP 4U
#define DCAM_STILL_STARTUP_SKIP 5U

struct dcam_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct ums9117_dcam_capture {
	struct device *dev;
	struct v4l2_device *v4l2;
	struct ums9117_jpeg_hw *hw;
	struct mutex *lock;
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev *sensor;
	struct video_device video;
	struct media_pad pad;
	struct vb2_queue queue;
	struct v4l2_pix_format format;
	spinlock_t qlock;
	struct list_head queued;
	struct dcam_buffer *active;
	struct work_struct work;
	struct delayed_work watchdog;
	struct mutex work_lock;
	u32 irq_events;
	unsigned long frame_deadline;
	u32 sequence;
	bool streaming;
	bool sensor_streaming;
	bool first_capture;
	bool registered;
	bool serial_g0;
	bool timed_out;
};

static struct ums9117_dcam_capture *
dcam_from_notifier(struct v4l2_async_notifier *notifier)
{
	return container_of(notifier, struct ums9117_dcam_capture, notifier);
}

static void dcam_pix_format(struct v4l2_pix_format *pix, u32 width, u32 height)
{
	memset(pix, 0, sizeof(*pix));
	pix->width = width;
	pix->height = height;
	pix->pixelformat = V4L2_PIX_FMT_NV16;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = width;
	pix->sizeimage = 2U * width * height;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int dcam_querycap(struct file *file, void *priv,
			 struct v4l2_capability *cap)
{
	strscpy(cap->driver, DCAM_NAME, sizeof(cap->driver));
	strscpy(cap->card, "UMS9117 camera", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" DCAM_NAME, sizeof(cap->bus_info));
	return 0;
}

static int dcam_enum_format(struct file *file, void *priv,
			    struct v4l2_fmtdesc *format)
{
	if (format->index)
		return -EINVAL;
	format->pixelformat = V4L2_PIX_FMT_NV16;
	return 0;
}

static int dcam_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *size)
{
	struct ums9117_dcam_capture *capture = video_drvdata(file);

	if (capture->serial_g0) {
		if (size->index || size->pixel_format != V4L2_PIX_FMT_NV16)
			return -EINVAL;
		size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		size->discrete.width = DCAM_SERIAL_WIDTH;
		size->discrete.height = DCAM_SERIAL_HEIGHT;
		return 0;
	}
	if (size->index > 1 || size->pixel_format != V4L2_PIX_FMT_NV16)
		return -EINVAL;
	size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	size->discrete.width = size->index ? DCAM_STILL_WIDTH :
					     DCAM_PREVIEW_WIDTH;
	size->discrete.height = size->index ? DCAM_STILL_HEIGHT :
					      DCAM_PREVIEW_HEIGHT;
	return 0;
}

static int dcam_get_format(struct file *file, void *priv,
			   struct v4l2_format *format)
{
	struct ums9117_dcam_capture *capture = video_drvdata(file);

	format->fmt.pix = capture->format;
	return 0;
}

static int dcam_try_format(struct file *file, void *priv,
			   struct v4l2_format *format)
{
	struct ums9117_dcam_capture *capture = video_drvdata(file);
	u32 width = DCAM_PREVIEW_WIDTH;
	u32 height = DCAM_PREVIEW_HEIGHT;

	if (capture->serial_g0) {
		width = DCAM_SERIAL_WIDTH;
		height = DCAM_SERIAL_HEIGHT;
	} else if (format->fmt.pix.width == DCAM_STILL_WIDTH &&
		   format->fmt.pix.height == DCAM_STILL_HEIGHT) {
		width = DCAM_STILL_WIDTH;
		height = DCAM_STILL_HEIGHT;
	}
	dcam_pix_format(&format->fmt.pix, width, height);
	return 0;
}

static int dcam_set_format(struct file *file, void *priv,
			   struct v4l2_format *format)
{
	struct ums9117_dcam_capture *capture = video_drvdata(file);

	if (vb2_is_busy(&capture->queue))
		return -EBUSY;
	dcam_try_format(file, priv, format);
	capture->format = format->fmt.pix;
	return 0;
}

static int dcam_enum_input(struct file *file, void *priv,
			   struct v4l2_input *input)
{
	if (input->index)
		return -EINVAL;
	strscpy(input->name, "Camera", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int dcam_get_input(struct file *file, void *priv, unsigned int *input)
{
	*input = 0;
	return 0;
}

static int dcam_set_input(struct file *file, void *priv, unsigned int input)
{
	return input ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops dcam_ioctl_ops = {
	.vidioc_querycap = dcam_querycap,
	.vidioc_enum_fmt_vid_cap = dcam_enum_format,
	.vidioc_enum_framesizes = dcam_enum_framesizes,
	.vidioc_g_fmt_vid_cap = dcam_get_format,
	.vidioc_try_fmt_vid_cap = dcam_try_format,
	.vidioc_s_fmt_vid_cap = dcam_set_format,
	.vidioc_enum_input = dcam_enum_input,
	.vidioc_g_input = dcam_get_input,
	.vidioc_s_input = dcam_set_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations dcam_file_ops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
};

static int dcam_queue_setup(struct vb2_queue *queue, unsigned int *buffers,
			    unsigned int *planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct ums9117_dcam_capture *capture = vb2_get_drv_priv(queue);
	u32 frame_bytes = capture->format.sizeimage;

	if (*planes) {
		if (*planes != 1 || sizes[0] < frame_bytes)
			return -EINVAL;
		return 0;
	}
	*planes = 1;
	sizes[0] = frame_bytes;
	return 0;
}

static int dcam_buffer_prepare(struct vb2_buffer *buffer)
{
	struct ums9117_dcam_capture *capture =
		vb2_get_drv_priv(buffer->vb2_queue);
	u32 frame_bytes = capture->format.sizeimage;

	if (vb2_plane_size(buffer, 0) < frame_bytes)
		return -EINVAL;
	vb2_set_plane_payload(buffer, 0, frame_bytes);
	return 0;
}

static void dcam_irq(void *data, u32 status)
{
	struct ums9117_dcam_capture *capture = data;
	unsigned long flags;

	spin_lock_irqsave(&capture->qlock, flags);
	capture->irq_events |= status;
	spin_unlock_irqrestore(&capture->qlock, flags);
	schedule_work(&capture->work);
}

static void dcam_watchdog(struct work_struct *work)
{
	struct ums9117_dcam_capture *capture = container_of(
		to_delayed_work(work), struct ums9117_dcam_capture, watchdog);
	unsigned long flags;

	spin_lock_irqsave(&capture->qlock, flags);
	if (capture->active && capture->streaming &&
	    time_after_eq(jiffies, capture->frame_deadline)) {
		capture->timed_out = true;
		schedule_work(&capture->work);
	}
	spin_unlock_irqrestore(&capture->qlock, flags);
}

static void dcam_work(struct work_struct *work)
{
	struct ums9117_dcam_capture *capture =
		container_of(work, struct ums9117_dcam_capture, work);
	struct dcam_buffer *buffer;
	unsigned long flags;
	unsigned int skip_frames;
	u32 events;
	bool timed_out;
	int ret;

	mutex_lock(&capture->work_lock);
	for (;;) {
		spin_lock_irqsave(&capture->qlock, flags);
		buffer = capture->active;
		events = capture->irq_events;
		capture->irq_events = 0;
		timed_out = capture->timed_out;
		capture->timed_out = false;
		spin_unlock_irqrestore(&capture->qlock, flags);
		if (buffer && (events || timed_out)) {
			cancel_delayed_work(&capture->watchdog);
			if (events)
				ret = ums9117_dcam_capture_finish(capture->hw);
			else
				ret = ums9117_dcam_capture_timeout(capture->hw);
			spin_lock_irqsave(&capture->qlock, flags);
			capture->active = NULL;
			spin_unlock_irqrestore(&capture->qlock, flags);
			buffer->vb.sequence = capture->sequence++;
			buffer->vb.vb2_buf.timestamp = ktime_get_ns();
			vb2_buffer_done(&buffer->vb.vb2_buf,
					ret ? VB2_BUF_STATE_ERROR :
					      VB2_BUF_STATE_DONE);
			if (ret && ums9117_jpeg_hw_failed(capture->hw))
				goto unlock;
			continue;
		}

		spin_lock_irqsave(&capture->qlock, flags);
		if (!capture->streaming || capture->active ||
		    list_empty(&capture->queued)) {
			spin_unlock_irqrestore(&capture->qlock, flags);
			goto unlock;
		}
		buffer = list_first_entry(&capture->queued, struct dcam_buffer,
					  list);
		list_del(&buffer->list);
		capture->active = buffer;
		spin_unlock_irqrestore(&capture->qlock, flags);
		skip_frames = 0;
		if (capture->first_capture && !capture->serial_g0)
			skip_frames = capture->format.width ==
						      DCAM_STILL_WIDTH ?
					      DCAM_STILL_STARTUP_SKIP :
					      DCAM_PREVIEW_STARTUP_SKIP;
		ret = ums9117_dcam_capture_start(
			capture->hw,
			vb2_dma_contig_plane_dma_addr(&buffer->vb.vb2_buf, 0),
			capture->format.width, capture->format.height,
			skip_frames, capture->serial_g0);
		if (!ret) {
			capture->first_capture = false;
			if (!capture->serial_g0)
				goto unlock;
			spin_lock_irqsave(&capture->qlock, flags);
			capture->frame_deadline =
				jiffies +
				msecs_to_jiffies(DCAM_SERIAL_FRAME_TIMEOUT_MS);
			spin_unlock_irqrestore(&capture->qlock, flags);
			mod_delayed_work(
				system_wq, &capture->watchdog,
				msecs_to_jiffies(DCAM_SERIAL_FRAME_TIMEOUT_MS));
			continue;
		}
		spin_lock_irqsave(&capture->qlock, flags);
		capture->active = NULL;
		spin_unlock_irqrestore(&capture->qlock, flags);
		vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		if (ums9117_jpeg_hw_failed(capture->hw))
			goto unlock;
	}
unlock:
	mutex_unlock(&capture->work_lock);
}

static void dcam_buffer_queue(struct vb2_buffer *buffer)
{
	struct ums9117_dcam_capture *capture =
		vb2_get_drv_priv(buffer->vb2_queue);
	struct dcam_buffer *queued = container_of(to_vb2_v4l2_buffer(buffer),
						  struct dcam_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&capture->qlock, flags);
	list_add_tail(&queued->list, &capture->queued);
	spin_unlock_irqrestore(&capture->qlock, flags);
	if (READ_ONCE(capture->streaming))
		schedule_work(&capture->work);
}

static void dcam_return_buffers(struct ums9117_dcam_capture *capture,
				enum vb2_buffer_state state)
{
	struct dcam_buffer *buffer;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&capture->qlock, flags);
		if (capture->active) {
			buffer = capture->active;
			capture->active = NULL;
		} else if (!list_empty(&capture->queued)) {
			buffer = list_first_entry(&capture->queued,
						  struct dcam_buffer, list);
			list_del(&buffer->list);
		} else {
			spin_unlock_irqrestore(&capture->qlock, flags);
			return;
		}
		spin_unlock_irqrestore(&capture->qlock, flags);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
}

static int dcam_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct ums9117_dcam_capture *capture = vb2_get_drv_priv(queue);
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
		.format = {
			.width = capture->format.width,
			.height = capture->format.height,
			.code = MEDIA_BUS_FMT_YUYV8_2X8,
			.field = V4L2_FIELD_NONE,
		},
	};
	int ret;

	if (!capture->sensor) {
		ret = -ENODEV;
		goto return_buffers;
	}
	ret = ums9117_dcam_claim(capture->hw, UMS9117_DCAM_CAPTURE, dcam_irq,
				 capture);
	if (ret)
		goto return_buffers;
	if (capture->serial_g0) {
		ret = ums9117_dcam_capture_route_serial_g0(capture->hw);
		if (ret)
			goto release;
	}
	ret = v4l2_subdev_call_state_active(capture->sensor, pad, set_fmt,
					    &format);
	if (ret)
		goto release;
	if (format.format.width != capture->format.width ||
	    format.format.height != capture->format.height ||
	    format.format.code != MEDIA_BUS_FMT_YUYV8_2X8) {
		ret = -EINVAL;
		goto release;
	}
	ret = v4l2_subdev_enable_streams(capture->sensor, 0, BIT_ULL(0));
	if (ret)
		goto release;
	capture->sensor_streaming = true;
	capture->sequence = 0;
	capture->first_capture = true;
	WRITE_ONCE(capture->streaming, true);
	schedule_work(&capture->work);
	return 0;

release:
	ums9117_dcam_release(capture->hw, UMS9117_DCAM_CAPTURE);
return_buffers:
	dcam_return_buffers(capture, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void dcam_stop_streaming(struct vb2_queue *queue)
{
	struct ums9117_dcam_capture *capture = vb2_get_drv_priv(queue);
	unsigned long flags;
	int ret;

	WRITE_ONCE(capture->streaming, false);
	cancel_delayed_work_sync(&capture->watchdog);
	mutex_lock(&capture->work_lock);
	ret = ums9117_dcam_capture_stop(capture->hw);
	spin_lock_irqsave(&capture->qlock, flags);
	capture->irq_events = 0;
	capture->timed_out = false;
	spin_unlock_irqrestore(&capture->qlock, flags);
	mutex_unlock(&capture->work_lock);
	cancel_work_sync(&capture->work);
	if (ret)
		dev_err(capture->dev, "capture stop needed reset: %pe\n",
			ERR_PTR(ret));
	dcam_return_buffers(capture, VB2_BUF_STATE_ERROR);
	if (capture->sensor_streaming) {
		v4l2_subdev_disable_streams(capture->sensor, 0, BIT_ULL(0));
		capture->sensor_streaming = false;
	}
	ums9117_dcam_release(capture->hw, UMS9117_DCAM_CAPTURE);
}

static const struct vb2_ops dcam_queue_ops = {
	.queue_setup = dcam_queue_setup,
	.buf_prepare = dcam_buffer_prepare,
	.buf_queue = dcam_buffer_queue,
	.start_streaming = dcam_start_streaming,
	.stop_streaming = dcam_stop_streaming,
};

static int dcam_bound(struct v4l2_async_notifier *notifier,
		      struct v4l2_subdev *sensor,
		      struct v4l2_async_connection *connection)
{
	struct ums9117_dcam_capture *capture = dcam_from_notifier(notifier);

	capture->sensor = sensor;
	return 0;
}

static int dcam_complete(struct v4l2_async_notifier *notifier)
{
	struct ums9117_dcam_capture *capture = dcam_from_notifier(notifier);
	int ret;

	if (!capture->sensor)
		return -ENODEV;
	capture->video.ctrl_handler = capture->sensor->ctrl_handler;
	ret = video_register_device(&capture->video, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;
	capture->registered = true;
	ret = media_create_pad_link(
		&capture->sensor->entity, 0, &capture->video.entity, 0,
		MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		video_unregister_device(&capture->video);
		capture->registered = false;
	}
	return ret;
}

static void dcam_unbind(struct v4l2_async_notifier *notifier,
			struct v4l2_subdev *sensor,
			struct v4l2_async_connection *connection)
{
	struct ums9117_dcam_capture *capture = dcam_from_notifier(notifier);

	if (capture->registered) {
		video_unregister_device(&capture->video);
		capture->registered = false;
	}
	capture->sensor = NULL;
}

static const struct v4l2_async_notifier_operations dcam_notifier_ops = {
	.bound = dcam_bound,
	.complete = dcam_complete,
	.unbind = dcam_unbind,
};

struct ums9117_dcam_capture *
ums9117_dcam_capture_create(struct device *dev, struct v4l2_device *v4l2,
			    struct ums9117_jpeg_hw *hw, struct mutex *lock)
{
	struct ums9117_dcam_capture *capture;
	struct v4l2_fwnode_endpoint endpoint = { 0 };
	struct v4l2_async_connection *connection;
	struct device_node *node;
	struct fwnode_handle *remote;
	u32 serial_width;
	bool serial_g0;
	int ret;

	node = of_graph_get_endpoint_by_regs(dev->of_node, 0, -1);
	if (!node)
		return NULL;
	remote = fwnode_graph_get_remote_endpoint(of_fwnode_handle(node));
	if (!remote) {
		of_node_put(node);
		return NULL;
	}
	fwnode_handle_put(remote);
	serial_g0 = of_property_present(node, "sprd,serial-g0-data-width");
	if (serial_g0) {
		ret = of_property_read_u32(node, "sprd,serial-g0-data-width",
					   &serial_width);
		if (ret)
			goto put_node;
		if (serial_width != 1 ||
		    of_property_present(node, "bus-type")) {
			ret = -EINVAL;
			goto put_node;
		}
	} else {
		ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(node),
						 &endpoint);
		if (ret)
			goto put_node;
		if (endpoint.bus_type != V4L2_MBUS_PARALLEL ||
		    endpoint.bus.parallel.bus_width != 8)
			ret = -EINVAL;
		v4l2_fwnode_endpoint_free(&endpoint);
		if (ret)
			goto put_node;
	}

	capture = kzalloc(sizeof(*capture), GFP_KERNEL);
	if (!capture) {
		ret = -ENOMEM;
		goto put_node;
	}
	capture->dev = dev;
	capture->v4l2 = v4l2;
	capture->hw = hw;
	capture->lock = lock;
	capture->serial_g0 = serial_g0;
	if (serial_g0)
		dcam_pix_format(&capture->format, DCAM_SERIAL_WIDTH,
				DCAM_SERIAL_HEIGHT);
	else
		dcam_pix_format(&capture->format, DCAM_PREVIEW_WIDTH,
				DCAM_PREVIEW_HEIGHT);
	spin_lock_init(&capture->qlock);
	mutex_init(&capture->work_lock);
	INIT_LIST_HEAD(&capture->queued);
	INIT_WORK(&capture->work, dcam_work);
	INIT_DELAYED_WORK(&capture->watchdog, dcam_watchdog);

	capture->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	capture->queue.io_modes = VB2_MMAP;
	capture->queue.drv_priv = capture;
	capture->queue.buf_struct_size = sizeof(struct dcam_buffer);
	capture->queue.ops = &dcam_queue_ops;
	capture->queue.mem_ops = &vb2_dma_contig_memops;
	capture->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	/* A single 1600x1200 NV16 buffer must fit the 4 MiB CMA pool. */
	capture->queue.min_queued_buffers = 0;
	capture->queue.lock = lock;
	capture->queue.dev = dev;
	ret = vb2_queue_init(&capture->queue);
	if (ret)
		goto free_capture;

	capture->video.fops = &dcam_file_ops;
	capture->video.ioctl_ops = &dcam_ioctl_ops;
	capture->video.v4l2_dev = v4l2;
	capture->video.queue = &capture->queue;
	capture->video.lock = lock;
	capture->video.release = video_device_release_empty;
	capture->video.vfl_dir = VFL_DIR_RX;
	capture->video.device_caps = V4L2_CAP_VIDEO_CAPTURE |
				     V4L2_CAP_STREAMING;
	capture->video.entity.function = MEDIA_ENT_F_IO_V4L;
	strscpy(capture->video.name, DCAM_NAME, sizeof(capture->video.name));
	video_set_drvdata(&capture->video, capture);
	capture->pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&capture->video.entity, 1, &capture->pad);
	if (ret)
		goto release_queue;

	v4l2_async_nf_init(&capture->notifier, v4l2);
	connection = v4l2_async_nf_add_fwnode_remote(
		&capture->notifier, of_fwnode_handle(node),
		struct v4l2_async_connection);
	if (IS_ERR(connection)) {
		ret = PTR_ERR(connection);
		goto cleanup_notifier;
	}
	capture->notifier.ops = &dcam_notifier_ops;
	ret = v4l2_async_nf_register(&capture->notifier);
	if (ret)
		goto cleanup_notifier;
	of_node_put(node);
	return capture;

cleanup_notifier:
	v4l2_async_nf_cleanup(&capture->notifier);
	media_entity_cleanup(&capture->video.entity);
release_queue:
	vb2_queue_release(&capture->queue);
free_capture:
	kfree(capture);
put_node:
	of_node_put(node);
	return ERR_PTR(ret);
}

void ums9117_dcam_capture_destroy(struct ums9117_dcam_capture *capture)
{
	if (!capture)
		return;
	cancel_delayed_work_sync(&capture->watchdog);
	v4l2_async_nf_unregister(&capture->notifier);
	v4l2_async_nf_cleanup(&capture->notifier);
	if (capture->registered)
		video_unregister_device(&capture->video);
	cancel_work_sync(&capture->work);
	media_entity_cleanup(&capture->video.entity);
	vb2_queue_release(&capture->queue);
	kfree(capture);
}
