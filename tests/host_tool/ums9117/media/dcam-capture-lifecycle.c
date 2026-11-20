// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>

#include "dcam-media.h"
#include "ums9117-dcam-capture.h"
#include "ums9117-dcam-format.h"
#include "ums9117-jpeg-hw.h"

/* Work is scheduled explicitly; this does not model simultaneous IRQs. */
struct ums9117_jpeg_hw {
	void (*irq)(void *data, u32 status);
	void (*ready)(void *data);
	void *callback_data;
	bool claimed;
	bool capturing;
	unsigned int starts;
	unsigned int stops;
	unsigned int releases;
};

struct buffer_result {
	unsigned int returns;
	enum vb2_buffer_state state;
};

static struct v4l2_async_notifier *registered_notifier;
static struct video_device *registered_video;
static struct work_struct *capture_work;
static struct delayed_work *capture_watchdog;
static struct v4l2_async_connection connection;
static struct device_node endpoint;
static struct fwnode_handle endpoint_handle;
static struct v4l2_subdev sensor;
static struct buffer_result results[3];
static unsigned int disables;
static unsigned int invalid_disables;
static unsigned int failures;

unsigned long jiffies;
const unsigned int vb2_dma_contig_memops;

#define CHECK(condition)                                                   \
	do {                                                               \
		if (!(condition)) {                                        \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition);                               \
			failures++;                                        \
		}                                                          \
	} while (0)

void dcam_test_init_work(struct work_struct *work,
			 void (*function)(struct work_struct *work))
{
	assert(!capture_work);
	work->function = function;
	work->pending = false;
	capture_work = work;
}

void dcam_test_init_delayed_work(struct delayed_work *work,
				 void (*function)(struct work_struct *work))
{
	assert(!capture_watchdog);
	work->work.function = function;
	work->work.pending = false;
	capture_watchdog = work;
}

bool schedule_work(struct work_struct *work)
{
	bool pending = work->pending;

	work->pending = true;
	return !pending;
}

bool cancel_work_sync(struct work_struct *work)
{
	bool pending = work->pending;

	work->pending = false;
	return pending;
}

bool mod_delayed_work(void *queue, struct delayed_work *work,
		      unsigned long delay)
{
	work->delay = delay;
	return !schedule_work(&work->work);
}

bool cancel_delayed_work(struct delayed_work *work)
{
	return cancel_work_sync(&work->work);
}

bool cancel_delayed_work_sync(struct delayed_work *work)
{
	return cancel_delayed_work(work);
}

static void run_capture_work(void)
{
	assert(capture_work->pending);
	capture_work->pending = false;
	capture_work->function(capture_work);
}

u64 ktime_get_ns(void)
{
	return 1000000;
}

struct device_node *of_graph_get_endpoint_by_regs(struct device_node *node,
						  int port, int index)
{
	assert(node && port == 0 && index == -1);
	return &endpoint;
}

struct fwnode_handle *of_fwnode_handle(struct device_node *node)
{
	assert(node == &endpoint);
	return &endpoint_handle;
}

struct fwnode_handle *
fwnode_graph_get_remote_endpoint(struct fwnode_handle *node)
{
	assert(node == &endpoint_handle);
	return &endpoint_handle;
}

void fwnode_handle_put(struct fwnode_handle *node)
{
	assert(node == &endpoint_handle);
}

void of_node_put(struct device_node *node)
{
	assert(node == &endpoint);
}

bool of_property_present(const struct device_node *node, const char *name)
{
	assert(node == &endpoint);
	return !strcmp(name, "sprd,serial-g0-data-width");
}

int of_property_read_u32(const struct device_node *node, const char *name,
			 u32 *value)
{
	assert(of_property_present(node, name));
	*value = 1;
	return 0;
}

int v4l2_fwnode_endpoint_parse(struct fwnode_handle *node,
			       struct v4l2_fwnode_endpoint *parsed)
{
	abort();
}

void v4l2_fwnode_endpoint_free(struct v4l2_fwnode_endpoint *parsed)
{
	abort();
}

void v4l2_async_nf_init(struct v4l2_async_notifier *notifier,
			struct v4l2_device *v4l2)
{
	assert(!registered_notifier);
}

struct v4l2_async_connection *
dcam_test_add_remote(struct v4l2_async_notifier *notifier,
		     struct fwnode_handle *remote)
{
	assert(remote == &endpoint_handle);
	return &connection;
}

int v4l2_async_nf_register(struct v4l2_async_notifier *notifier)
{
	registered_notifier = notifier;
	return 0;
}

void v4l2_async_nf_unregister(struct v4l2_async_notifier *notifier)
{
	assert(notifier == registered_notifier);
	registered_notifier = NULL;
}

void v4l2_async_nf_cleanup(struct v4l2_async_notifier *notifier)
{
}

int dcam_test_get_fmt(struct v4l2_subdev *subdev,
		      struct v4l2_subdev_format *format)
{
	assert(subdev == &sensor);
	format->format.width = 240;
	format->format.height = 320;
	format->format.code = 0x2008;
	return 0;
}

int dcam_test_set_fmt(struct v4l2_subdev *subdev,
		      struct v4l2_subdev_format *format)
{
	assert(subdev == &sensor);
	assert(format->format.width == 240 && format->format.height == 320);
	assert(format->format.code == 0x2008);
	return 0;
}

int v4l2_subdev_enable_streams(struct v4l2_subdev *subdev, u32 pad, u64 streams)
{
	assert(subdev == &sensor && !sensor.streaming);
	assert(pad == 0 && streams == 1);
	sensor.streaming = true;
	return 0;
}

int v4l2_subdev_disable_streams(struct v4l2_subdev *subdev, u32 pad,
				u64 streams)
{
	disables++;
	if (subdev != &sensor || !sensor.streaming) {
		invalid_disables++;
		return -ENODEV;
	}
	assert(pad == 0 && streams == 1);
	sensor.streaming = false;
	return 0;
}

int media_entity_pads_init(struct media_entity *entity, unsigned int count,
			   struct media_pad *pads)
{
	assert(count == 1);
	return 0;
}

void media_entity_cleanup(struct media_entity *entity)
{
}

int media_create_pad_link(struct media_entity *source, u32 source_pad,
			  struct media_entity *sink, u32 sink_pad, u32 flags)
{
	assert(source == &sensor.entity && source_pad == 0 && sink_pad == 0);
	return 0;
}

int video_register_device(struct video_device *video, int type, int number)
{
	assert(!registered_video && !video->registered);
	registered_video = video;
	video->registered = true;
	return 0;
}

void video_unregister_device(struct video_device *video)
{
	video->registered = false;
}

/* Model the queue/owner cleanup promised by the vb2 unregister boundary. */
void vb2_video_unregister_device(struct video_device *video)
{
	struct mutex *lock;

	if (!video || !video->registered)
		return;
	video_unregister_device(video);
	assert(video->queue);
	lock = video->queue->lock ? video->queue->lock : video->lock;
	if (lock)
		mutex_lock(lock);
	vb2_queue_release(video->queue);
	video->queue->owner = NULL;
	if (lock)
		mutex_unlock(lock);
}

void video_device_release_empty(struct video_device *video)
{
}

int vb2_queue_init(struct vb2_queue *queue)
{
	assert(queue->buf_struct_size >= sizeof(struct vb2_v4l2_buffer));
	return 0;
}

void vb2_queue_release(struct vb2_queue *queue)
{
	unsigned int i;

	if (queue->streaming) {
		queue->ops->stop_streaming(queue);
		queue->streaming = false;
	}
	for (i = 0; i < queue->buffer_count; i++) {
		free(queue->buffers[i]);
		queue->buffers[i] = NULL;
	}
	queue->buffer_count = 0;
}

void vb2_buffer_done(struct vb2_buffer *buffer, enum vb2_buffer_state state)
{
	assert(buffer->index < 3);
	results[buffer->index].returns++;
	results[buffer->index].state = state;
}

int v4l2_fh_open(struct file *file)
{
	assert(file->video->registered);
	file->private_data = file;
	return 0;
}

int vb2_fop_release(struct file *file)
{
	struct vb2_queue *queue = file->video->queue;

	mutex_lock(queue->lock);
	if (!queue->owner || queue->owner == file->private_data) {
		vb2_queue_release(queue);
		queue->owner = NULL;
	}
	mutex_unlock(queue->lock);
	file->private_data = NULL;
	return 0;
}

int vb2_ioctl_reqbufs(struct file *file, void *private,
		      struct v4l2_requestbuffers *request)
{
	struct vb2_queue *queue = file->video->queue;
	unsigned int planes = 0, sizes[1], i;
	int ret;

	assert(request->count == 3 && !queue->buffer_count);
	ret = queue->ops->queue_setup(queue, &request->count, &planes, sizes,
				      NULL);
	if (ret)
		return ret;
	assert(planes == 1 && sizes[0] == 153600);
	for (i = 0; i < request->count; i++) {
		struct vb2_buffer *buffer = calloc(1, queue->buf_struct_size);

		assert(buffer);
		buffer->vb2_queue = queue;
		buffer->index = i;
		buffer->plane_size = sizes[0];
		queue->buffers[i] = buffer;
	}
	queue->buffer_count = request->count;
	queue->owner = file->private_data;
	return 0;
}

int vb2_ioctl_qbuf(struct file *file, void *private,
		   struct v4l2_buffer *request)
{
	struct vb2_queue *queue = file->video->queue;
	struct vb2_buffer *buffer;
	int ret;

	assert(request->index < queue->buffer_count);
	buffer = queue->buffers[request->index];
	ret = queue->ops->buf_prepare(buffer);
	if (ret)
		return ret;
	assert(buffer->payload == 153600);
	queue->ops->buf_queue(buffer);
	return 0;
}

int vb2_ioctl_streamon(struct file *file, void *private, unsigned int type)
{
	struct vb2_queue *queue = file->video->queue;
	int ret;

	assert(type == V4L2_BUF_TYPE_VIDEO_CAPTURE && !queue->streaming);
	ret = queue->ops->start_streaming(queue, queue->buffer_count);
	if (!ret)
		queue->streaming = true;
	return ret;
}

/* These registered operations are outside this lifecycle scenario. */
#define UNUSED_IOCTL(name)                                    \
	int name(struct file *file, void *private, void *arg) \
	{                                                     \
		abort();                                      \
	}
UNUSED_IOCTL(vb2_ioctl_create_bufs)
UNUSED_IOCTL(vb2_ioctl_prepare_buf)
UNUSED_IOCTL(vb2_ioctl_querybuf)
UNUSED_IOCTL(vb2_ioctl_dqbuf)
UNUSED_IOCTL(vb2_ioctl_expbuf)

int vb2_ioctl_streamoff(struct file *file, void *private, unsigned int type)
{
	abort();
}

unsigned int vb2_fop_poll(struct file *file, void *wait)
{
	abort();
}

long video_ioctl2(struct file *file, unsigned int command, unsigned long arg)
{
	abort();
}

int vb2_fop_mmap(struct file *file, void *mapping)
{
	abort();
}

int ums9117_dcam_format_configure(u32 width, u32 height, u32 code,
				  unsigned int serial_width,
				  struct ums9117_dcam_format *format)
{
	assert(width == 240 && height == 320 && code == 0x2008);
	assert(serial_width == 1);
	memset(format, 0, sizeof(*format));
	return 0;
}

int ums9117_dcam_capture_register(struct ums9117_jpeg_hw *hw,
				  void (*irq)(void *, u32),
				  void (*ready)(void *), void *data)
{
	assert(!hw->irq && !hw->ready);
	hw->irq = irq;
	hw->ready = ready;
	hw->callback_data = data;
	return 0;
}

void ums9117_dcam_capture_unregister(struct ums9117_jpeg_hw *hw)
{
	hw->irq = NULL;
	hw->ready = NULL;
	hw->callback_data = NULL;
}

int ums9117_dcam_claim(struct ums9117_jpeg_hw *hw,
		       enum ums9117_dcam_owner owner)
{
	assert(owner == UMS9117_DCAM_CAPTURE && !hw->claimed);
	hw->claimed = true;
	return 0;
}

void ums9117_dcam_release(struct ums9117_jpeg_hw *hw,
			  enum ums9117_dcam_owner owner)
{
	assert(owner == UMS9117_DCAM_CAPTURE && hw->claimed && !hw->capturing);
	hw->claimed = false;
	hw->releases++;
}

int ums9117_dcam_capture_route_serial_g0(struct ums9117_jpeg_hw *hw)
{
	assert(hw->claimed);
	return 0;
}

int ums9117_dcam_capture_start(struct ums9117_jpeg_hw *hw, dma_addr_t dma,
			       u32 width, u32 height, unsigned int skip_frames,
			       unsigned int serial_width, u32 code)
{
	assert(hw->claimed && !hw->capturing);
	assert(width == 240 && height == 320 && serial_width == 1);
	assert(dma == 0x100000 && skip_frames == 0 && code == 0x2008);
	hw->capturing = true;
	hw->starts++;
	return 0;
}

int ums9117_dcam_capture_stop(struct ums9117_jpeg_hw *hw)
{
	assert(hw->capturing);
	hw->capturing = false;
	hw->stops++;
	return 0;
}

int ums9117_dcam_capture_finish(struct ums9117_jpeg_hw *hw)
{
	abort();
}

int ums9117_dcam_capture_timeout(struct ums9117_jpeg_hw *hw)
{
	abort();
}

bool ums9117_jpeg_hw_failed(struct ums9117_jpeg_hw *hw)
{
	return false;
}

static void check_stopped(struct ums9117_jpeg_hw *hw)
{
	unsigned int i;

	CHECK(!registered_video->registered);
	CHECK(!registered_video->queue->streaming);
	CHECK(!registered_video->queue->owner);
	CHECK(!registered_video->queue->buffer_count);
	CHECK(!hw->claimed && !hw->capturing);
	CHECK(!hw->irq && !hw->ready && !hw->callback_data);
	CHECK(hw->starts == 1 && hw->stops == 1 && hw->releases == 1);
	CHECK(!capture_work->pending);
	CHECK(!capture_watchdog->work.pending);
	CHECK(!sensor.streaming);
	CHECK(disables == 1 && invalid_disables == 0);
	for (i = 0; i < 3; i++) {
		CHECK(results[i].returns == 1);
		CHECK(results[i].state == VB2_BUF_STATE_ERROR);
	}
}

int main(void)
{
	struct device_node root_node = {};
	struct device device = { .of_node = &root_node };
	struct v4l2_device v4l2 = {};
	struct ums9117_jpeg_hw hw = {};
	struct ums9117_dcam_capture *capture;
	struct v4l2_requestbuffers request = { .count = 3 };
	struct mutex lock = {};
	struct file file = {};
	unsigned int i;

	capture = ums9117_dcam_capture_create(&device, &v4l2, &hw, &lock);
	assert(capture && !IS_ERR(capture));
	assert(registered_notifier && capture_work && capture_watchdog);
	assert(registered_notifier->ops->bound(registered_notifier, &sensor,
					       &connection) == 0);
	assert(registered_notifier->ops->complete(registered_notifier) == 0);
	file.video = registered_video;
	assert(file.video->fops->open(&file) == 0);
	assert(file.video->ioctl_ops->vidioc_reqbufs(&file, file.private_data,
						     &request) == 0);
	for (i = 0; i < 3; i++) {
		struct v4l2_buffer buffer = { .index = i };

		assert(file.video->ioctl_ops->vidioc_qbuf(
			       &file, file.private_data, &buffer) == 0);
	}
	assert(file.video->ioctl_ops->vidioc_streamon(
		       &file, file.private_data, V4L2_BUF_TYPE_VIDEO_CAPTURE) ==
	       0);
	run_capture_work();
	assert(hw.claimed && hw.capturing && sensor.streaming);
	assert(hw.starts == 1 && capture_watchdog->work.pending);
	assert(capture_watchdog->delay == 3000);
	assert(!capture_work->pending && hw.ready && hw.irq);
	hw.irq(hw.callback_data, 0x10);
	assert(capture_work->pending);
	for (i = 0; i < 3; i++)
		assert(!results[i].returns);

	registered_notifier->ops->unbind(registered_notifier, &sensor,
					 &connection);
	check_stopped(&hw);
	assert(file.video->fops->release(&file) == 0);
	check_stopped(&hw);
	ums9117_dcam_capture_destroy(capture);
	CHECK(!registered_notifier);
	CHECK(disables == 1 && invalid_disables == 0);
	if (failures)
		return 1;
	puts("DCAM capture lifecycle checks passed");
	return 0;
}
