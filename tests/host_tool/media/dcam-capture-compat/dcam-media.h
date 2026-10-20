/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_DCAM_MEDIA_H
#define FPLINUX_TEST_DCAM_MEDIA_H

#include "dcam-host.h"

#define V4L2_PIX_FMT_NV16 0x3631564e
#define V4L2_FIELD_NONE 1
#define V4L2_COLORSPACE_SRGB 8
#define V4L2_YCBCR_ENC_DEFAULT 0
#define V4L2_QUANTIZATION_FULL_RANGE 1
#define V4L2_XFER_FUNC_DEFAULT 0
#define V4L2_FRMSIZE_TYPE_DISCRETE 1
#define V4L2_INPUT_TYPE_CAMERA 2
#define V4L2_SUBDEV_FORMAT_ACTIVE 1
#define V4L2_MBUS_PARALLEL 1
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC 0x2000
#define V4L2_CAP_VIDEO_CAPTURE 1
#define V4L2_CAP_STREAMING 0x04000000
#define VB2_MMAP 1
#define VFL_TYPE_VIDEO 0
#define VFL_DIR_RX 0
#define MEDIA_LNK_FL_ENABLED 1
#define MEDIA_LNK_FL_IMMUTABLE 2
#define MEDIA_ENT_F_IO_V4L 1
#define MEDIA_PAD_FL_SINK 1

struct v4l2_pix_format {
	u32 width, height, pixelformat, field, bytesperline, sizeimage;
	u32 colorspace, ycbcr_enc, quantization, xfer_func;
};
struct v4l2_format {
	union {
		struct v4l2_pix_format pix;
	} fmt;
};
struct v4l2_capability {
	char driver[16], card[32], bus_info[32];
};
struct v4l2_fmtdesc {
	u32 index, pixelformat;
};
struct v4l2_frmsizeenum {
	u32 index, pixel_format, type;
	struct {
		u32 width, height;
	} discrete;
};
struct v4l2_input {
	u32 index, type;
	char name[32];
};
struct media_entity {
	u32 function;
};
struct media_pad {
	u32 flags;
};
struct v4l2_subdev {
	void *ctrl_handler;
	struct media_entity entity;
	bool streaming;
};
struct v4l2_device {
	int unused;
};
struct v4l2_subdev_format {
	u32 which, pad;
	struct {
		u32 width, height, code, field;
	} format;
};
struct v4l2_fwnode_endpoint {
	u32 bus_type;
	struct {
		struct {
			u32 bus_width;
		} parallel;
	} bus;
};
struct v4l2_async_connection {
	int unused;
};
struct v4l2_async_notifier;
struct v4l2_async_notifier_operations {
	int (*bound)(struct v4l2_async_notifier *, struct v4l2_subdev *,
		     struct v4l2_async_connection *);
	int (*complete)(struct v4l2_async_notifier *);
	void (*unbind)(struct v4l2_async_notifier *, struct v4l2_subdev *,
		       struct v4l2_async_connection *);
};
struct v4l2_async_notifier {
	const struct v4l2_async_notifier_operations *ops;
};

struct vb2_queue;
enum vb2_buffer_state {
	VB2_BUF_STATE_QUEUED,
	VB2_BUF_STATE_DONE,
	VB2_BUF_STATE_ERROR,
};
struct vb2_buffer {
	struct vb2_queue *vb2_queue;
	u64 timestamp;
	size_t plane_size, payload;
	unsigned int index;
};
struct vb2_v4l2_buffer {
	struct vb2_buffer vb2_buf;
	u32 sequence;
};
struct vb2_ops {
	int (*queue_setup)(struct vb2_queue *, unsigned int *, unsigned int *,
			   unsigned int[], struct device *[]);
	int (*buf_prepare)(struct vb2_buffer *);
	void (*buf_queue)(struct vb2_buffer *);
	int (*start_streaming)(struct vb2_queue *, unsigned int);
	void (*stop_streaming)(struct vb2_queue *);
};
struct vb2_queue {
	u32 type, io_modes, timestamp_flags, min_queued_buffers;
	size_t buf_struct_size;
	void *drv_priv, *owner;
	const struct vb2_ops *ops;
	const void *mem_ops;
	struct mutex *lock;
	struct device *dev;
	bool streaming;
	unsigned int buffer_count;
	struct vb2_buffer *buffers[3];
};
struct video_device;
struct file {
	struct video_device *video;
	void *private_data;
};
struct v4l2_requestbuffers {
	unsigned int count;
};
struct v4l2_buffer {
	unsigned int index;
};
struct v4l2_file_operations {
	void *owner;
	int (*open)(struct file *);
	int (*release)(struct file *);
	unsigned int (*poll)(struct file *, void *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	int (*mmap)(struct file *, void *);
};
struct v4l2_ioctl_ops {
	int (*vidioc_querycap)(struct file *, void *, struct v4l2_capability *);
	int (*vidioc_enum_fmt_vid_cap)(struct file *, void *,
				       struct v4l2_fmtdesc *);
	int (*vidioc_enum_framesizes)(struct file *, void *,
				      struct v4l2_frmsizeenum *);
	int (*vidioc_g_fmt_vid_cap)(struct file *, void *,
				    struct v4l2_format *);
	int (*vidioc_try_fmt_vid_cap)(struct file *, void *,
				      struct v4l2_format *);
	int (*vidioc_s_fmt_vid_cap)(struct file *, void *,
				    struct v4l2_format *);
	int (*vidioc_enum_input)(struct file *, void *, struct v4l2_input *);
	int (*vidioc_g_input)(struct file *, void *, unsigned int *);
	int (*vidioc_s_input)(struct file *, void *, unsigned int);
	int (*vidioc_reqbufs)(struct file *, void *,
			      struct v4l2_requestbuffers *);
	int (*vidioc_create_bufs)(struct file *, void *, void *);
	int (*vidioc_prepare_buf)(struct file *, void *, void *);
	int (*vidioc_querybuf)(struct file *, void *, void *);
	int (*vidioc_qbuf)(struct file *, void *, struct v4l2_buffer *);
	int (*vidioc_dqbuf)(struct file *, void *, void *);
	int (*vidioc_expbuf)(struct file *, void *, void *);
	int (*vidioc_streamon)(struct file *, void *, unsigned int);
	int (*vidioc_streamoff)(struct file *, void *, unsigned int);
};
struct video_device {
	const struct v4l2_file_operations *fops;
	const struct v4l2_ioctl_ops *ioctl_ops;
	struct v4l2_device *v4l2_dev;
	struct vb2_queue *queue;
	struct mutex *lock;
	void (*release)(struct video_device *);
	u32 vfl_dir, device_caps;
	struct media_entity entity;
	void *ctrl_handler, *driver_data;
	char name[32];
	bool registered;
};

extern const unsigned int vb2_dma_contig_memops;
int vb2_queue_init(struct vb2_queue *queue);
void vb2_queue_release(struct vb2_queue *queue);
void vb2_buffer_done(struct vb2_buffer *buffer, enum vb2_buffer_state state);
#define vb2_get_drv_priv(queue) ((queue)->drv_priv)
#define vb2_is_busy(queue) ((queue)->buffer_count != 0)
#define vb2_plane_size(buffer, plane) ((buffer)->plane_size)
#define vb2_set_plane_payload(buffer, plane, size) ((buffer)->payload = (size))
#define to_vb2_v4l2_buffer(buffer) \
	container_of(buffer, struct vb2_v4l2_buffer, vb2_buf)
#define vb2_dma_contig_plane_dma_addr(buffer, plane) \
	((dma_addr_t)(0x100000 + (buffer)->index * 0x100000))

int video_register_device(struct video_device *video, int type, int number);
void video_unregister_device(struct video_device *video);
void vb2_video_unregister_device(struct video_device *video);
void video_device_release_empty(struct video_device *video);
#define video_set_drvdata(video, data) ((video)->driver_data = (data))
#define video_drvdata(file) ((file)->video->driver_data)
int v4l2_fh_open(struct file *file);
int vb2_fop_release(struct file *file);
unsigned int vb2_fop_poll(struct file *file, void *wait);
long video_ioctl2(struct file *file, unsigned int command, unsigned long arg);
int vb2_fop_mmap(struct file *file, void *mapping);
int vb2_ioctl_reqbufs(struct file *, void *, struct v4l2_requestbuffers *);
int vb2_ioctl_qbuf(struct file *, void *, struct v4l2_buffer *);
int vb2_ioctl_streamon(struct file *, void *, unsigned int);
int vb2_ioctl_streamoff(struct file *, void *, unsigned int);
int vb2_ioctl_create_bufs(struct file *, void *, void *);
int vb2_ioctl_prepare_buf(struct file *, void *, void *);
int vb2_ioctl_querybuf(struct file *, void *, void *);
int vb2_ioctl_dqbuf(struct file *, void *, void *);
int vb2_ioctl_expbuf(struct file *, void *, void *);

void v4l2_async_nf_init(struct v4l2_async_notifier *, struct v4l2_device *);
struct v4l2_async_connection *dcam_test_add_remote(struct v4l2_async_notifier *,
						   struct fwnode_handle *);
#define v4l2_async_nf_add_fwnode_remote(notifier, remote, type) \
	dcam_test_add_remote(notifier, remote)
int v4l2_async_nf_register(struct v4l2_async_notifier *notifier);
void v4l2_async_nf_unregister(struct v4l2_async_notifier *notifier);
void v4l2_async_nf_cleanup(struct v4l2_async_notifier *notifier);
int dcam_test_get_fmt(struct v4l2_subdev *, struct v4l2_subdev_format *);
int dcam_test_set_fmt(struct v4l2_subdev *, struct v4l2_subdev_format *);
#define v4l2_subdev_call_state_active(sensor, group, operation, format) \
	dcam_test_##operation(sensor, format)
int v4l2_subdev_enable_streams(struct v4l2_subdev *, u32 pad, u64 streams);
int v4l2_subdev_disable_streams(struct v4l2_subdev *, u32 pad, u64 streams);
int v4l2_fwnode_endpoint_parse(struct fwnode_handle *,
			       struct v4l2_fwnode_endpoint *);
void v4l2_fwnode_endpoint_free(struct v4l2_fwnode_endpoint *endpoint);
int media_entity_pads_init(struct media_entity *, unsigned int,
			   struct media_pad *);
void media_entity_cleanup(struct media_entity *entity);
int media_create_pad_link(struct media_entity *, u32, struct media_entity *,
			  u32, u32);

#endif
