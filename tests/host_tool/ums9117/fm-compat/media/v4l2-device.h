/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FM_HOST_V4L2_DEVICE_H
#define FPLINUX_FM_HOST_V4L2_DEVICE_H

#include <fcntl.h>
#include <linux/module.h>
#include <linux/mutex.h>

/* Registration/callback facade only; no V4L2 core or device I/O is run. */
struct video_device;
struct v4l2_ctrl;
struct file {
	int f_flags;
	struct video_device *video;
};
struct v4l2_device {
	struct device *dev;
	void (*release)(struct v4l2_device *);
};
struct v4l2_ctrl_ops {
	int (*s_ctrl)(struct v4l2_ctrl *);
};
struct v4l2_ctrl_handler {
	int error;
	struct v4l2_ctrl *control;
};
struct v4l2_ctrl {
	struct v4l2_ctrl_handler *handler;
	const struct v4l2_ctrl_ops *ops;
	int val;
};
struct v4l2_capability {
	char driver[16], card[32], bus_info[32];
};
struct v4l2_tuner {
	u32 index, type, capability, rangelow, rangehigh, audmode;
	u32 signal, rxsubchans;
	int afc;
	char name[32];
};
struct v4l2_frequency {
	u32 tuner, type, frequency;
};
struct v4l2_hw_freq_seek {
	u32 tuner, type, seek_upward, wrap_around, spacing, rangelow, rangehigh;
};
struct v4l2_file_operations {
	void *owner;
	int (*open)(struct file *);
	int (*release)(struct file *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
};
struct v4l2_ioctl_ops {
	int (*vidioc_querycap)(struct file *, void *, struct v4l2_capability *);
	int (*vidioc_g_tuner)(struct file *, void *, struct v4l2_tuner *);
	int (*vidioc_s_tuner)(struct file *, void *, const struct v4l2_tuner *);
	int (*vidioc_g_frequency)(struct file *, void *,
				  struct v4l2_frequency *);
	int (*vidioc_s_frequency)(struct file *, void *,
				  const struct v4l2_frequency *);
	int (*vidioc_s_hw_freq_seek)(struct file *, void *,
				     const struct v4l2_hw_freq_seek *);
};
struct video_device {
	char name[32];
	struct v4l2_device *v4l2_dev;
	struct v4l2_ctrl_handler *ctrl_handler;
	const struct v4l2_file_operations *fops;
	const struct v4l2_ioctl_ops *ioctl_ops;
	struct mutex *lock;
	void (*release)(struct video_device *);
	u32 device_caps;
	void *data;
	bool registered;
};

#define V4L2_TUNER_RADIO 1
#define V4L2_TUNER_CAP_LOW 0x0001
#define V4L2_TUNER_CAP_STEREO 0x0010
#define V4L2_TUNER_CAP_HWSEEK_BOUNDED 0x0004
#define V4L2_TUNER_MODE_STEREO 1
#define V4L2_CAP_RADIO 0x00040000
#define V4L2_CAP_TUNER 0x00010000
#define V4L2_CAP_HW_FREQ_SEEK 0x00000400
#define V4L2_CID_AUDIO_MUTE 0x00980909
#define VFL_TYPE_RADIO 2

int v4l2_device_register(struct device *dev, struct v4l2_device *v4l2);
void v4l2_device_unregister(struct v4l2_device *v4l2);
void v4l2_device_disconnect(struct v4l2_device *v4l2);
void v4l2_device_put(struct v4l2_device *v4l2);
int video_register_device(struct video_device *video, int type, int number);
void video_unregister_device(struct video_device *video);
void video_device_release_empty(struct video_device *video);
int v4l2_fh_open(struct file *file);
int v4l2_fh_release(struct file *file);
long video_ioctl2(struct file *file, unsigned int request, unsigned long arg);
void v4l2_ctrl_handler_init(struct v4l2_ctrl_handler *handler,
			    unsigned int count);
void v4l2_ctrl_handler_free(struct v4l2_ctrl_handler *handler);
struct v4l2_ctrl *v4l2_ctrl_new_std(struct v4l2_ctrl_handler *handler,
				    const struct v4l2_ctrl_ops *ops, u32 id,
				    int min, int max, int step, int value);
int v4l2_ctrl_s_ctrl(struct v4l2_ctrl *control, int value);

static inline void video_set_drvdata(struct video_device *video, void *data)
{
	video->data = data;
}
static inline void *video_drvdata(struct file *file)
{
	return file->video->data;
}
static inline bool video_is_registered(struct video_device *video)
{
	return video->registered;
}

#endif
