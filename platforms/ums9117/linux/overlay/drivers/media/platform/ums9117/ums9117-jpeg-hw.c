// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/ums9117/mmio.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#include "ums9117-jpeg-codec.h"
#include "ums9117-dcam-format.h"
#include "ums9117-jpeg-hw-internal.h"

static void jpeg_prepare_job(struct ums9117_jpeg_hw *hw);

u32 ums9117_jpeg_read(struct ums9117_jpeg_hw *hw, u32 reg)
{
	return readl(hw->base + reg);
}

void ums9117_jpeg_write(struct ums9117_jpeg_hw *hw, u32 reg, u32 value)
{
	writel(value, hw->base + reg);
}

static int jpeg_wait_idle(struct ums9117_jpeg_hw *hw, u32 *status)
{
	return readl_poll_timeout(hw->base + JPEG_AHB_STATUS, *status,
				  !(*status & JPEG_AHB_BUSY), 10, JPEG_POLL_US);
}

static int jpeg_set_clock_selector(struct ums9117_jpeg_hw *hw, u32 selector)
{
	u32 clock = readl(hw->clock);

	writel((clock & ~JPEG_CLOCK_SELECTOR_MASK) | selector, hw->clock);
	return (readl(hw->clock) & JPEG_CLOCK_SELECTOR_MASK) == selector ? 0 :
									   -EIO;
}

static void jpeg_snapshot(struct ums9117_jpeg_hw *hw)
{
	unsigned long flags;
	struct ums9117_jpeg_hw_stats snapshot = {
		.last_status = ums9117_jpeg_read(hw, JPEG_INT_STATUS),
		.last_raw = ums9117_jpeg_read(hw, JPEG_INT_RAW),
		.last_mbio = ums9117_jpeg_read(hw, JPEG_MBIO_STATUS),
		.last_progress = ums9117_jpeg_read(hw, JPEG_MBIO_PROGRESS),
		.last_ahb = ums9117_jpeg_read(hw, JPEG_AHB_STATUS),
		.last_bsm_cfg0 = ums9117_jpeg_read(hw, JPEG_BSM_CFG0),
		.last_bsm_cfg1 = ums9117_jpeg_read(hw, JPEG_BSM_CFG1),
		.last_bsm_debug = ums9117_jpeg_read(hw, JPEG_BSM_DEBUG),
		.last_vld = ums9117_jpeg_read(hw, JPEG_VLD_CONTROL),
	};

	spin_lock_irqsave(&hw->lock, flags);
	hw->stats.last_status = snapshot.last_status | hw->events;
	hw->stats.last_raw = snapshot.last_raw | hw->irq_raw;
	hw->stats.last_mbio = snapshot.last_mbio;
	hw->stats.last_progress = snapshot.last_progress;
	hw->stats.last_ahb = snapshot.last_ahb;
	hw->stats.last_bsm_cfg0 = snapshot.last_bsm_cfg0;
	hw->stats.last_bsm_cfg1 = snapshot.last_bsm_cfg1;
	hw->stats.last_bsm_debug = snapshot.last_bsm_debug;
	hw->stats.last_vld = snapshot.last_vld;
	spin_unlock_irqrestore(&hw->lock, flags);
}

static u32 jpeg_operation_errors(enum ums9117_jpeg_hw_operation operation)
{
	switch (operation) {
	case JPEG_OPERATION_DECODE:
		return JPEG_DECODE_IRQ_ERRORS;
	case JPEG_OPERATION_ENCODE:
		return JPEG_ENCODE_IRQ_ERRORS;
	case JPEG_OPERATION_SCALE:
		return JPEG_SCALE_IRQ_ERRORS;
	default:
		return 0;
	}
}

static u32 jpeg_operation_completion(enum ums9117_jpeg_hw_operation operation)
{
	return operation == JPEG_OPERATION_SCALE ? JPEG_SCALE_IRQ_DONE :
						   JPEG_IRQ_DONE;
}

static irqreturn_t jpeg_irq(int irq, void *data)
{
	struct ums9117_jpeg_hw *hw = data;
	enum ums9117_jpeg_hw_operation operation = READ_ONCE(hw->operation);
	u32 mask = READ_ONCE(hw->active_irq_mask);
	u32 status = ums9117_jpeg_read(hw, JPEG_INT_STATUS) & mask;
	u32 raw;
	void (*capture_irq)(void *data, u32 status);
	void *capture_data;
	unsigned long flags;

	if (!status)
		return IRQ_NONE;
	raw = ums9117_jpeg_read(hw, JPEG_INT_RAW);
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, status);

	spin_lock_irqsave(&hw->lock, flags);
	hw->events |= status;
	hw->irq_raw |= raw;
	hw->operation_events |= status | (raw & mask);
	hw->active_irq_count++;
	if (operation > JPEG_OPERATION_NONE &&
	    operation < JPEG_OPERATION_COUNT) {
		struct ums9117_jpeg_hw_operation_stats *stats =
			&hw->stats.operation[operation];

		stats->irqs++;
		if (status & jpeg_operation_completion(operation))
			stats->completion_irqs++;
		if (status & jpeg_operation_errors(operation))
			stats->error_irqs++;
	}
	if (operation == JPEG_OPERATION_DECODE && (status & JPEG_IRQ_BSM))
		hw->stats.bsm_irqs++;
	spin_unlock_irqrestore(&hw->lock, flags);
	capture_irq = READ_ONCE(hw->capture_irq);
	capture_data = READ_ONCE(hw->capture_irq_data);
	if (capture_irq && READ_ONCE(hw->owner_active) &&
	    READ_ONCE(hw->owner) == UMS9117_DCAM_CAPTURE &&
	    operation == JPEG_OPERATION_NONE) {
		ums9117_jpeg_write(hw, JPEG_CFG,
				   ums9117_jpeg_read(hw, JPEG_CFG) &
					   ~DCAM_CAP_ENABLE);
		ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
		ums9117_jpeg_read(hw, JPEG_INT_MASK);
		WRITE_ONCE(hw->active_irq_mask, 0);
		capture_irq(capture_data, status);
		return IRQ_HANDLED;
	}

	if (operation != JPEG_OPERATION_DECODE ||
	    status & (JPEG_IRQ_DONE | JPEG_DECODE_IRQ_ERRORS))
		complete(&hw->completion);
	return IRQ_HANDLED;
}

static bool jpeg_codec_can_claim(struct ums9117_jpeg_hw *hw)
{
	return !READ_ONCE(hw->owner_active) &&
	       (!READ_ONCE(hw->capture_pending) || READ_ONCE(hw->codec_turn));
}

/* owner_lock serializes ready notifications with capture unregister. */
static void jpeg_notify_owner_locked(struct ums9117_jpeg_hw *hw)
{
	wake_up_all(&hw->owner_wait);
	if (hw->capture_pending && hw->capture_ready)
		hw->capture_ready(hw->capture_irq_data);
}

int ums9117_jpeg_hw_streamon(struct ums9117_jpeg_hw *hw)
{
	int ret = 0;

	mutex_lock(&hw->owner_lock);
	if (ums9117_jpeg_hw_failed(hw))
		ret = -EIO;
	else
		hw->codec_users++;
	mutex_unlock(&hw->owner_lock);
	return ret;
}

void ums9117_jpeg_hw_streamoff(struct ums9117_jpeg_hw *hw)
{
	mutex_lock(&hw->owner_lock);
	if (WARN_ON(!hw->codec_users))
		goto unlock;
	if (!--hw->codec_users && !ums9117_jpeg_hw_failed(hw)) {
		/* M2M stops the final job before releasing its streaming queues. */
		if (WARN_ON(hw->owner_active &&
			    hw->owner == UMS9117_DCAM_CODEC))
			goto unlock;
		ums9117_jpeg_free_buffers(hw);
	}
unlock:
	mutex_unlock(&hw->owner_lock);
}

int ums9117_dcam_capture_register(struct ums9117_jpeg_hw *hw,
				  void (*irq)(void *data, u32 status),
				  void (*ready)(void *data), void *data)
{
	int ret = 0;

	mutex_lock(&hw->owner_lock);
	if (ums9117_jpeg_hw_failed(hw)) {
		ret = -EIO;
		goto unlock;
	}
	if (hw->capture_irq || !irq || !ready) {
		ret = -EBUSY;
		goto unlock;
	}
	hw->capture_irq_data = data;
	hw->capture_ready = ready;
	WRITE_ONCE(hw->capture_irq, irq);
unlock:
	mutex_unlock(&hw->owner_lock);
	return ret;
}

void ums9117_dcam_capture_unregister(struct ums9117_jpeg_hw *hw)
{
	mutex_lock(&hw->owner_lock);
	WARN_ON(hw->owner_active && hw->owner == UMS9117_DCAM_CAPTURE);
	WRITE_ONCE(hw->capture_pending, false);
	hw->capture_ready = NULL;
	WRITE_ONCE(hw->capture_irq, NULL);
	synchronize_irq(hw->irq);
	hw->capture_irq_data = NULL;
	wake_up_all(&hw->owner_wait);
	mutex_unlock(&hw->owner_lock);
}

int ums9117_dcam_claim(struct ums9117_jpeg_hw *hw,
		       enum ums9117_dcam_owner owner)
{
	u32 clock, status;
	unsigned long remaining = msecs_to_jiffies(JPEG_WAIT_MS);
	int ret = 0;

	mutex_lock(&hw->owner_lock);
	if (owner == UMS9117_DCAM_CODEC) {
		if (WARN_ON(hw->codec_waiting || !hw->codec_users)) {
			ret = -EBUSY;
			goto unlock;
		}
		hw->codec_waiting = true;
		for (;;) {
			if (atomic_read(&hw->cancelled)) {
				ret = -ECANCELED;
				goto codec_done;
			}
			if (ums9117_jpeg_hw_failed(hw)) {
				ret = -EIO;
				goto codec_done;
			}
			if (jpeg_codec_can_claim(hw))
				break;
			mutex_unlock(&hw->owner_lock);
			remaining = wait_event_timeout(
				hw->owner_wait,
				atomic_read(&hw->cancelled) ||
					ums9117_jpeg_hw_failed(hw) ||
					jpeg_codec_can_claim(hw),
				remaining);
			mutex_lock(&hw->owner_lock);
			if (!remaining) {
				ret = -ETIMEDOUT;
				goto codec_done;
			}
		}
		ret = jpeg_wait_idle(hw, &status);
		if (ret)
			goto codec_done;
		hw->codec_saved_clock = readl(hw->clock) &
					JPEG_CLOCK_SELECTOR_MASK;
		ret = jpeg_set_clock_selector(hw, JPEG_CODEC_CLOCK_SELECTOR);
		if (ret) {
			if (jpeg_set_clock_selector(hw, hw->codec_saved_clock))
				WRITE_ONCE(hw->failed, true);
			goto codec_done;
		}
		WRITE_ONCE(hw->owner, owner);
		WRITE_ONCE(hw->owner_active, true);
		jpeg_prepare_job(hw);
codec_done:
		hw->codec_waiting = false;
		if (ret)
			jpeg_notify_owner_locked(hw);
		goto unlock;
	}
	if (ums9117_jpeg_hw_failed(hw)) {
		ret = -EIO;
		goto unlock;
	}
	if (owner != UMS9117_DCAM_CAPTURE || !hw->capture_irq) {
		ret = -EINVAL;
		goto unlock;
	}
	WRITE_ONCE(hw->capture_pending, true);
	if (hw->owner_active || (hw->codec_waiting && hw->codec_turn)) {
		ret = -EAGAIN;
		goto unlock;
	}
	WRITE_ONCE(hw->capture_pending, false);
	ret = regmap_read(hw->aon_apb, DCAM_CCIR_ROUTE,
			  &hw->capture_saved_route);
	if (ret)
		goto unlock;
	ret = regmap_update_bits(hw->aon_apb, DCAM_CCIR_ROUTE,
				 DCAM_CCIR_ROUTE_PARALLEL,
				 DCAM_CCIR_ROUTE_PARALLEL);
	if (ret)
		goto unlock;
	clock = readl(hw->clock);
	hw->capture_saved_clock = clock & JPEG_CLOCK_MASK;
	writel((clock & ~GENMASK(1, 0)) | 1, hw->clock);
	WRITE_ONCE(hw->owner, owner);
	WRITE_ONCE(hw->owner_active, true);
unlock:
	mutex_unlock(&hw->owner_lock);
	return ret;
}

#if IS_ENABLED(CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0)
int ums9117_dcam_capture_route_serial_g0(struct ums9117_jpeg_hw *hw)
{
	if (WARN_ON(!hw->owner_active || hw->owner != UMS9117_DCAM_CAPTURE))
		return -EINVAL;
	return regmap_update_bits(hw->aon_apb, DCAM_CCIR_ROUTE,
				  DCAM_CCIR_ROUTE_PARALLEL, 0);
}
#endif

void ums9117_dcam_release(struct ums9117_jpeg_hw *hw,
			  enum ums9117_dcam_owner owner)
{
	u32 clock, status;
	int ret;

	mutex_lock(&hw->owner_lock);
	if (WARN_ON(!hw->owner_active || hw->owner != owner))
		goto unlock;
	if (owner == UMS9117_DCAM_CODEC) {
		if (!ums9117_jpeg_hw_failed(hw)) {
			ret = jpeg_wait_idle(hw, &status);
			if (!ret)
				ret = jpeg_set_clock_selector(
					hw, hw->codec_saved_clock);
			if (ret) {
				WRITE_ONCE(hw->failed, true);
				dev_err_ratelimited(
					hw->dev,
					"codec clock restore failed: %pe\n",
					ERR_PTR(ret));
			}
		}
	} else {
		if (!ums9117_jpeg_hw_failed(hw)) {
			clock = readl(hw->clock);
			writel((clock & ~JPEG_CLOCK_MASK) |
				       hw->capture_saved_clock,
			       hw->clock);
			ret = regmap_update_bits(hw->aon_apb, DCAM_CCIR_ROUTE,
						 DCAM_CCIR_ROUTE_PARALLEL,
						 hw->capture_saved_route);
			if (ret)
				WRITE_ONCE(hw->failed, true);
		}
	}
	WRITE_ONCE(hw->codec_turn, owner == UMS9117_DCAM_CAPTURE);
	WRITE_ONCE(hw->owner_active, false);
	jpeg_notify_owner_locked(hw);
unlock:
	mutex_unlock(&hw->owner_lock);
}

int ums9117_dcam_capture_start(struct ums9117_jpeg_hw *hw, dma_addr_t dma,
			       u32 width, u32 height, unsigned int skip_frames,
			       unsigned int serial_data_width, u32 mbus_code)
{
	struct ums9117_dcam_format format;
	u32 capture_control;
	u32 status;
	int ret;

	if (WARN_ON(!hw->owner_active || hw->owner != UMS9117_DCAM_CAPTURE))
		return -EINVAL;
	if ((serial_data_width &&
	     !IS_ENABLED(CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0)) ||
	    (!serial_data_width &&
	     !IS_ENABLED(CONFIG_VIDEO_UMS9117_DCAM_PARALLEL)) ||
	    skip_frames > 15)
		return -EINVAL;
	ret = ums9117_dcam_format_configure(width, height, mbus_code,
					    serial_data_width, &format);
	if (ret)
		return ret;
	if (dma & 3 || dma > U32_MAX - 2 * format.y_bytes)
		return -EINVAL;
	ret = jpeg_wait_idle(hw, &status);
	if (ret)
		return ret;

	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_write(hw, JPEG_CFG, 0);
	writel(JPEG_DCAM_RESET | JPEG_CCIR_RESET, hw->reset_set);
	writel(JPEG_DCAM_RESET | JPEG_CCIR_RESET, hw->reset_clear);
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	spin_lock_irq(&hw->lock);
	hw->events = 0;
	hw->irq_raw = 0;
	spin_unlock_irq(&hw->lock);

	capture_control = format.capture_control;
	if (IS_ENABLED(CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0_TWO_BIT) &&
	    serial_data_width == 2) {
		/* Preserve reset state outside the selected transport fields. */
		capture_control |= ums9117_jpeg_read(hw, DCAM_CAP_CTRL) &
				   ~DCAM_CAP_TRANSPORT_MASK;
	}
	ums9117_jpeg_write(hw, DCAM_CAP_CTRL, capture_control);
	ums9117_jpeg_write(hw, DCAM_CAP_FRM_CNT,
			   DCAM_CAP_FRAME_CLEAR | skip_frames);
	ums9117_jpeg_write(hw, JPEG_SOURCE_SIZE, format.source_size);
	ums9117_jpeg_write(hw, DCAM_CAP_START, format.capture_start);
	ums9117_jpeg_write(hw, DCAM_CAP_END, format.capture_end);
	ums9117_jpeg_write(hw, DCAM_CAP_DECIMATION, 0);
	ums9117_jpeg_write(hw, DCAM_SPI_WIDTH, 2 * width - 1);
	ums9117_jpeg_write(hw, JPEG_SC_FORMAT, 0);
	ums9117_jpeg_write(hw, JPEG_DEST_SIZE, format.source_size);
	ums9117_jpeg_write(hw, JPEG_SCALING_CONFIG, BIT(1));
	ums9117_jpeg_write(hw, JPEG_ENDIAN, BIT(2));
	ums9117_jpeg_write(hw, JPEG_FRAME0_Y, lower_32_bits(dma));
	ums9117_jpeg_write(hw, JPEG_FRAME0_UV,
			   lower_32_bits(dma + format.y_bytes));
	ums9117_jpeg_write(hw, JPEG_FRAME1_Y, 0);
	ums9117_jpeg_write(hw, JPEG_FRAME1_UV, 0);
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	ums9117_jpeg_write(hw, JPEG_INT_MASK, DCAM_CAPTURE_IRQS);
	WRITE_ONCE(hw->active_irq_mask, DCAM_CAPTURE_IRQS);
	dma_wmb();
	ums9117_jpeg_write(hw, DCAM_AUTO_COPY, 1);
	ums9117_jpeg_write(hw, DCAM_AUTO_COPY, 1);
	ums9117_jpeg_write(hw, DCAM_AUTO_COPY, 0);
	ums9117_jpeg_write(hw, DCAM_AUTO_COPY, BIT(1));
	ums9117_jpeg_write(hw, JPEG_CFG, DCAM_CAP_ENABLE);
	return 0;
}

int ums9117_dcam_capture_stop(struct ums9117_jpeg_hw *hw)
{
	u32 status;
	int ret, reset_ret;

	if (WARN_ON(!hw->owner_active || hw->owner != UMS9117_DCAM_CAPTURE))
		return -EINVAL;
	ums9117_jpeg_write(hw, JPEG_CFG,
			   ums9117_jpeg_read(hw, JPEG_CFG) & ~DCAM_CAP_ENABLE);
	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	WRITE_ONCE(hw->active_irq_mask, 0);
	ret = jpeg_wait_idle(hw, &status);
	if (ret) {
		writel(JPEG_DCAM_RESET | JPEG_CCIR_RESET, hw->reset_set);
		writel(JPEG_DCAM_RESET | JPEG_CCIR_RESET, hw->reset_clear);
		reset_ret = jpeg_wait_idle(hw, &status);
		if (reset_ret) {
			WRITE_ONCE(hw->failed, true);
			ret = reset_ret;
		}
	}
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	return ret;
}

int ums9117_dcam_capture_finish(struct ums9117_jpeg_hw *hw)
{
	u32 events, raw, ahb, cfg, cap, frames;
	int ret, result;

	ahb = ums9117_jpeg_read(hw, JPEG_AHB_STATUS);
	cfg = ums9117_jpeg_read(hw, JPEG_CFG);
	cap = ums9117_jpeg_read(hw, DCAM_CAP_CTRL);
	frames = ums9117_jpeg_read(hw, DCAM_CAP_FRM_CNT);
	ret = ums9117_dcam_capture_stop(hw);
	spin_lock_irq(&hw->lock);
	events = hw->events;
	raw = hw->irq_raw;
	spin_unlock_irq(&hw->lock);
	if (!ret && (events & DCAM_CAPTURE_IRQS) == DCAM_CAPTURE_DONE)
		return 0;
	result = ret ?: -EIO;
	dev_err_ratelimited(hw->dev, "capture failed: %pe\n", ERR_PTR(result));
	dev_dbg(hw->dev,
		"capture status: events=%08x raw=%08x ahb=%08x cfg=%08x cap=%08x frames=%08x stop=%d\n",
		events, raw, ahb, cfg, cap, frames, ret);
	return result;
}

#if IS_ENABLED(CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0)
int ums9117_dcam_capture_timeout(struct ums9117_jpeg_hw *hw)
{
	u32 status = ums9117_jpeg_read(hw, JPEG_INT_STATUS);
	u32 raw = ums9117_jpeg_read(hw, JPEG_INT_RAW);
	u32 ahb = ums9117_jpeg_read(hw, JPEG_AHB_STATUS);
	u32 cap = ums9117_jpeg_read(hw, DCAM_CAP_CTRL);
	u32 frames = ums9117_jpeg_read(hw, DCAM_CAP_FRM_CNT);
	int ret = ums9117_dcam_capture_stop(hw);
	int result = ret ?: -ETIMEDOUT;

	dev_err_ratelimited(hw->dev, "capture timed out: %pe\n",
			    ERR_PTR(result));
	dev_dbg(hw->dev,
		"capture timeout status: status=%08x raw=%08x ahb=%08x cap=%08x frames=%08x stop=%d\n",
		status, raw, ahb, cap, frames, ret);
	return result;
}
#endif

void *ums9117_jpeg_buffer_data(struct ums9117_jpeg_dma_buffer *buffer)
{
	return (u8 *)buffer->allocation + JPEG_GUARD_BYTES;
}

dma_addr_t ums9117_jpeg_buffer_dma(struct ums9117_jpeg_dma_buffer *buffer)
{
	return buffer->dma + JPEG_GUARD_BYTES;
}

static void jpeg_free_buffer(struct ums9117_jpeg_hw *hw,
			     struct ums9117_jpeg_dma_buffer *buffer)
{
	if (buffer->allocation)
		dma_free_coherent(hw->dev, buffer->allocation_size,
				  buffer->allocation, buffer->dma);
	memset(buffer, 0, sizeof(*buffer));
}

void ums9117_jpeg_free_buffers(struct ums9117_jpeg_hw *hw)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hw->buffers); ++i)
		jpeg_free_buffer(hw, &hw->buffers[i]);
}

static int jpeg_allocate_buffer(struct ums9117_jpeg_hw *hw,
				struct ums9117_jpeg_dma_buffer *buffer,
				size_t size)
{
	buffer->size = ALIGN(size, JPEG_GUARD_BYTES);
	buffer->allocation_size = buffer->size + 2 * JPEG_GUARD_BYTES;
	buffer->allocation = dma_alloc_coherent(
		hw->dev, buffer->allocation_size, &buffer->dma, GFP_KERNEL);
	if (!buffer->allocation)
		return -ENOMEM;
	if (upper_32_bits(buffer->dma + buffer->allocation_size - 1))
		return -ERANGE;
	memset(buffer->allocation, JPEG_GUARD_VALUE, JPEG_GUARD_BYTES);
	memset((u8 *)buffer->allocation + JPEG_GUARD_BYTES + buffer->size,
	       JPEG_GUARD_VALUE, JPEG_GUARD_BYTES);
	memset(ums9117_jpeg_buffer_data(buffer), 0, buffer->size);
	return 0;
}

int ums9117_jpeg_prepare_buffers(struct ums9117_jpeg_hw *hw,
				 const size_t sizes[4])
{
	unsigned int i;
	int ret;

	/* Discard mismatched banks before allocating their replacements. */
	for (i = 0; i < ARRAY_SIZE(hw->buffers); ++i) {
		struct ums9117_jpeg_dma_buffer *buffer = &hw->buffers[i];

		if (!sizes[i] ||
		    buffer->size != ALIGN(sizes[i], JPEG_GUARD_BYTES))
			jpeg_free_buffer(hw, buffer);
	}
	for (i = 0; i < ARRAY_SIZE(hw->buffers); ++i) {
		struct ums9117_jpeg_dma_buffer *buffer = &hw->buffers[i];

		if (!sizes[i])
			continue;
		if (buffer->allocation) {
			memset(ums9117_jpeg_buffer_data(buffer), 0,
			       buffer->size);
			continue;
		}
		ret = jpeg_allocate_buffer(hw, buffer, sizes[i]);
		if (ret)
			return ret;
	}
	return 0;
}

bool ums9117_jpeg_guards_ok(struct ums9117_jpeg_hw *hw)
{
	unsigned int i;

	dma_rmb();
	for (i = 0; i < ARRAY_SIZE(hw->buffers); ++i) {
		struct ums9117_jpeg_dma_buffer *buffer = &hw->buffers[i];
		u8 *allocation = buffer->allocation;

		if (!allocation)
			continue;
		if (memchr_inv(allocation, JPEG_GUARD_VALUE,
			       JPEG_GUARD_BYTES) ||
		    memchr_inv(allocation + JPEG_GUARD_BYTES + buffer->size,
			       JPEG_GUARD_VALUE, JPEG_GUARD_BYTES))
			return false;
	}
	return true;
}

static bool jpeg_dma_overlaps(const struct ums9117_jpeg_dma_buffer *left,
			      const struct ums9117_jpeg_dma_buffer *right)
{
	u64 left_start = left->dma;
	u64 right_start = right->dma;
	u64 left_end = left_start + left->allocation_size;
	u64 right_end = right_start + right->allocation_size;

	return left_start < right_end && right_start < left_end;
}

bool ums9117_jpeg_buffers_disjoint(struct ums9117_jpeg_hw *hw)
{
	unsigned int left;

	for (left = 0; left < ARRAY_SIZE(hw->buffers); ++left) {
		unsigned int right;

		if (!hw->buffers[left].allocation)
			continue;
		for (right = left + 1; right < ARRAY_SIZE(hw->buffers);
		     ++right) {
			if (!hw->buffers[right].allocation)
				continue;
			if (jpeg_dma_overlaps(&hw->buffers[left],
					      &hw->buffers[right]))
				return false;
		}
	}
	return true;
}

u32 ums9117_jpeg_events(struct ums9117_jpeg_hw *hw)
{
	unsigned long flags;
	u32 events;

	spin_lock_irqsave(&hw->lock, flags);
	events = hw->events;
	spin_unlock_irqrestore(&hw->lock, flags);
	return events;
}

void ums9117_jpeg_encode_snapshot_timeout(struct ums9117_jpeg_hw *hw,
					  bool poll_valid, u32 poll_offset,
					  u32 poll_mask, u32 poll_expected)
{
	struct ums9117_jpeg_encode_timeout_stats snapshot = {
		.valid = true,
		.poll_valid = poll_valid,
		.strip = READ_ONCE(hw->active_encode_strip),
		.poll_offset = poll_offset,
		.poll_mask = poll_mask,
		.poll_expected = poll_expected,
		.poll_value = poll_valid ? ums9117_jpeg_read(hw, poll_offset) :
					   0,
		.bsm_debug = ums9117_jpeg_read(hw, JPEG_BSM_DEBUG),
		.bsm_write_ready = ums9117_jpeg_read(hw, JPEG_BSM_WRITE_READY),
		.bsm_bits = ums9117_jpeg_read(hw, JPEG_BSM_BITS),
		.vlc_control = ums9117_jpeg_read(hw, JPEG_VLC_CONTROL),
		.ahb = ums9117_jpeg_read(hw, JPEG_AHB_STATUS),
	};
	unsigned long flags;

	spin_lock_irqsave(&hw->lock, flags);
	snapshot.irq_events = hw->events;
	snapshot.irq_raw = hw->irq_raw;
	hw->stats.encode_timeout = snapshot;
	spin_unlock_irqrestore(&hw->lock, flags);
}

void ums9117_jpeg_set_operation(struct ums9117_jpeg_hw *hw,
				enum ums9117_jpeg_hw_operation operation,
				u32 irq_mask)
{
	unsigned long flags;

	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	reinit_completion(&hw->completion);
	spin_lock_irqsave(&hw->lock, flags);
	hw->events = 0;
	hw->irq_raw = 0;
	hw->active_irq_count = 0;
	hw->operation_events = 0;
	spin_unlock_irqrestore(&hw->lock, flags);
	WRITE_ONCE(hw->operation, operation);
	WRITE_ONCE(hw->active_irq_mask, irq_mask);
}

void ums9117_jpeg_clear_operation(struct ums9117_jpeg_hw *hw)
{
	WRITE_ONCE(hw->active_irq_mask, 0);
	WRITE_ONCE(hw->operation, JPEG_OPERATION_NONE);
}

void ums9117_jpeg_reset_submission(struct ums9117_jpeg_hw *hw)
{
	unsigned long flags;

	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	reinit_completion(&hw->completion);
	spin_lock_irqsave(&hw->lock, flags);
	hw->events = 0;
	hw->irq_raw = 0;
	spin_unlock_irqrestore(&hw->lock, flags);
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
}

unsigned long ums9117_jpeg_subdeadline(unsigned long deadline)
{
	unsigned long local = jiffies + usecs_to_jiffies(JPEG_POLL_US);

	return time_before(local, deadline) ? local : deadline;
}

int ums9117_jpeg_operation_check(struct ums9117_jpeg_hw *hw, u32 error_mask,
				 u32 full_mask, unsigned long deadline)
{
	u32 events = ums9117_jpeg_events(hw) |
		     (ums9117_jpeg_read(hw, JPEG_INT_RAW) &
		      READ_ONCE(hw->active_irq_mask));

	if (atomic_read(&hw->cancelled))
		return -ECANCELED;
	if (events & error_mask)
		return -EIO;
	if (events & full_mask)
		return -ENOSPC;
	if (time_after_eq(jiffies, deadline))
		return -ETIMEDOUT;
	return 0;
}

int ums9117_jpeg_operation_poll(struct ums9117_jpeg_hw *hw, u32 reg, u32 mask,
				u32 expected, u32 error_mask, u32 full_mask,
				unsigned long deadline)
{
	int ret;

	for (;;) {
		ret = ums9117_jpeg_operation_check(hw, error_mask, full_mask,
						   deadline);
		if (ret) {
			if (ret == -ETIMEDOUT &&
			    READ_ONCE(hw->operation) == JPEG_OPERATION_ENCODE)
				ums9117_jpeg_encode_snapshot_timeout(
					hw, true, reg, mask, expected);
			return ret;
		}
		if ((ums9117_jpeg_read(hw, reg) & mask) == expected)
			return 0;
		usleep_range(10, 20);
	}
}

void ums9117_jpeg_operation_request(struct ums9117_jpeg_hw *hw,
				    enum ums9117_jpeg_hw_operation operation)
{
	unsigned long flags;

	spin_lock_irqsave(&hw->lock, flags);
	hw->operation_events = 0;
	hw->stats.operation[operation].requests++;
	spin_unlock_irqrestore(&hw->lock, flags);
}

void ums9117_jpeg_operation_result(struct ums9117_jpeg_hw *hw,
				   enum ums9117_jpeg_hw_operation operation,
				   int result, u64 units, u64 bytes,
				   u64 started_ns)
{
	struct ums9117_jpeg_hw_operation_stats *stats;
	unsigned long flags;
	u32 ahb = ums9117_jpeg_read(hw, JPEG_AHB_STATUS);

	spin_lock_irqsave(&hw->lock, flags);
	stats = &hw->stats.operation[operation];
	stats->last_result = result;
	stats->last_duration_ns = ktime_get_ns() - started_ns;
	stats->last_events = hw->operation_events;
	stats->last_ahb = ahb;
	stats->last_bytes = result ? 0 : bytes;
	stats->units += units;
	if (!result)
		stats->completed++;
	if (result == -ETIMEDOUT)
		stats->timeouts++;
	if (result == -ECANCELED)
		stats->cancellations++;
	spin_unlock_irqrestore(&hw->lock, flags);
}

void ums9117_jpeg_reset(struct ums9117_jpeg_hw *hw)
{
	writel(JPEG_RESET, hw->reset_set);
	writel(JPEG_RESET, hw->reset_clear);
}

int ums9117_jpeg_stop(struct ums9117_jpeg_hw *hw)
{
	enum ums9117_jpeg_hw_operation operation = READ_ONCE(hw->operation);
	unsigned long flags;
	u32 status;
	int ret;

	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	readl(hw->base + JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	if (operation == JPEG_OPERATION_DECODE)
		jpeg_snapshot(hw);
	ums9117_jpeg_write(hw, JPEG_BSM_CFG0,
			   ums9117_jpeg_read(hw, JPEG_BSM_CFG0) &
				   ~JPEG_BSM_BUFFERS);
	ums9117_jpeg_write(hw, JPEG_MBIO_ARM, 0);
	ums9117_jpeg_reset(hw);
	ret = jpeg_wait_idle(hw, &status);
	if (!ret)
		ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);

	spin_lock_irqsave(&hw->lock, flags);
	if (operation > JPEG_OPERATION_NONE && operation < JPEG_OPERATION_COUNT)
		hw->stats.operation[operation].resets++;
	if (operation == JPEG_OPERATION_DECODE)
		hw->stats.recovery_ahb = status;
	if (ret) {
		if (operation > JPEG_OPERATION_NONE &&
		    operation < JPEG_OPERATION_COUNT)
			hw->stats.operation[operation].idle_failures++;
		WRITE_ONCE(hw->failed, true);
	}
	spin_unlock_irqrestore(&hw->lock, flags);
	ums9117_jpeg_clear_operation(hw);
	return ret;
}

void ums9117_jpeg_hw_prepare(struct ums9117_jpeg_hw *hw)
{
	atomic_set(&hw->cancelled, 0);
}

static void jpeg_prepare_job(struct ums9117_jpeg_hw *hw)
{
	unsigned long flags;

	reinit_completion(&hw->completion);
	spin_lock_irqsave(&hw->lock, flags);
	hw->events = 0;
	hw->irq_raw = 0;
	hw->active_irq_count = 0;
	hw->operation_events = 0;
	spin_unlock_irqrestore(&hw->lock, flags);
	WRITE_ONCE(hw->active_irq_mask, 0);
	WRITE_ONCE(hw->operation, JPEG_OPERATION_NONE);
}

void ums9117_jpeg_hw_cancel(struct ums9117_jpeg_hw *hw)
{
	atomic_set(&hw->cancelled, 1);
	complete_all(&hw->completion);
	wake_up_all(&hw->owner_wait);
}

bool ums9117_jpeg_hw_failed(struct ums9117_jpeg_hw *hw)
{
	return READ_ONCE(hw->failed);
}

static void jpeg_restore_clock_gate(struct ums9117_jpeg_hw *hw)
{
	u32 value = readl(hw->clock);

	writel((value & ~JPEG_CLOCK_MASK) | hw->saved_clock, hw->clock);
	if (!hw->saved_gate)
		writel(JPEG_GATE, hw->gate_clear);
}

static void jpeg_restore_qos(struct ums9117_jpeg_hw *hw)
{
	u32 value = readl(hw->dcam_qos);

	writel((value & ~DCAM_QOS_MASK) | hw->saved_qos, hw->dcam_qos);
	readl(hw->dcam_qos);
}

static int jpeg_prepare_qos(struct ums9117_jpeg_hw *hw)
{
	u32 value = readl(hw->dcam_qos);
	u32 cpu = readl(hw->cpu_qos);
	u32 qos;

	hw->saved_qos = value & DCAM_QOS_MASK;
	/* Match CPU QoS to prevent capture overflow during buffer preparation. */
	qos = FIELD_PREP(DCAM_QOS_READ_MASK,
			 FIELD_GET(CPU_QOS_READ_MASK, cpu)) |
	      FIELD_PREP(DCAM_QOS_WRITE_MASK,
			 FIELD_GET(CPU_QOS_WRITE_MASK, cpu));
	writel((value & ~DCAM_QOS_MASK) | qos, hw->dcam_qos);
	return (readl(hw->dcam_qos) & DCAM_QOS_MASK) == qos ? 0 : -EIO;
}

struct ums9117_jpeg_hw *ums9117_jpeg_hw_create(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ums9117_jpeg_hw *hw;
	struct resource *resource;
	u32 status, value;
	int ret;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "jpeg");
	if (!resource || resource->start != 0x20c00000 ||
	    resource_size(resource) != JPEG_MMIO_BYTES)
		return ERR_PTR(-EINVAL);
	hw = devm_kzalloc(dev, sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return ERR_PTR(-ENOMEM);
	hw->dev = dev;
	hw->base = devm_ioremap_resource(dev, resource);
	if (IS_ERR(hw->base))
		return ERR_CAST(hw->base);
	hw->gate_state = ums9117_devm_ioremap_shared_register(
		pdev, "ap-ahb-gate-state", 0x20e00000);
	hw->gate_set = ums9117_devm_ioremap_shared_register(
		pdev, "ap-ahb-gate-set", 0x20e01000);
	hw->gate_clear = ums9117_devm_ioremap_shared_register(
		pdev, "ap-ahb-gate-clear", 0x20e02000);
	hw->reset_set = ums9117_devm_ioremap_shared_register(
		pdev, "ap-ahb-reset-set", 0x20e01004);
	hw->reset_clear = ums9117_devm_ioremap_shared_register(
		pdev, "ap-ahb-reset-clear", 0x20e02004);
	hw->clock = ums9117_devm_ioremap_shared_register(pdev, "dcam-clock",
							 0x21500088);
	hw->cpu_qos = ums9117_devm_ioremap_shared_register(pdev, "cpu-qos",
							   0x20e000a4);
	hw->dcam_qos = ums9117_devm_ioremap_shared_register(pdev, "dcam-qos",
							    0x20e03058);
	if (IS_ERR(hw->gate_state))
		return ERR_CAST(hw->gate_state);
	if (IS_ERR(hw->gate_set))
		return ERR_CAST(hw->gate_set);
	if (IS_ERR(hw->gate_clear))
		return ERR_CAST(hw->gate_clear);
	if (IS_ERR(hw->reset_set))
		return ERR_CAST(hw->reset_set);
	if (IS_ERR(hw->reset_clear))
		return ERR_CAST(hw->reset_clear);
	if (IS_ERR(hw->clock))
		return ERR_CAST(hw->clock);
	if (IS_ERR(hw->cpu_qos))
		return ERR_CAST(hw->cpu_qos);
	if (IS_ERR(hw->dcam_qos))
		return ERR_CAST(hw->dcam_qos);
	hw->aon_apb =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,aon-apb");
	if (IS_ERR(hw->aon_apb))
		return ERR_CAST(hw->aon_apb);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ERR_PTR(ret);
	hw->irq = platform_get_irq(pdev, 0);
	if (hw->irq < 0)
		return ERR_PTR(hw->irq);
	spin_lock_init(&hw->lock);
	mutex_init(&hw->owner_lock);
	init_waitqueue_head(&hw->owner_wait);
	init_completion(&hw->completion);
	atomic_set(&hw->cancelled, 0);
	hw->stats.operation[JPEG_OPERATION_DECODE].last_result = -ENODATA;
	hw->stats.operation[JPEG_OPERATION_ENCODE].last_result = -ENODATA;
	hw->stats.operation[JPEG_OPERATION_SCALE].last_result = -ENODATA;
	hw->saved_gate = readl(hw->gate_state) & JPEG_GATE;
	hw->saved_clock = readl(hw->clock) & JPEG_CLOCK_MASK;
	writel(JPEG_GATE, hw->gate_set);
	readl(hw->gate_state);
	ret = jpeg_wait_idle(hw, &status);
	if (ret) {
		if (!hw->saved_gate)
			writel(JPEG_GATE, hw->gate_clear);
		return ERR_PTR(ret);
	}
	value = readl(hw->clock);
	writel(value & ~JPEG_CLOCK_MASK, hw->clock);
	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	ret = devm_request_irq(dev, hw->irq, jpeg_irq, 0, dev_name(dev), hw);
	if (ret) {
		jpeg_restore_clock_gate(hw);
		return ERR_PTR(ret);
	}
	ret = jpeg_prepare_qos(hw);
	if (ret) {
		jpeg_restore_qos(hw);
		devm_free_irq(dev, hw->irq, hw);
		jpeg_restore_clock_gate(hw);
		return ERR_PTR(ret);
	}
	ums9117_jpeg_stats_create(hw);
	return hw;
}

void ums9117_jpeg_hw_destroy(struct ums9117_jpeg_hw *hw)
{
	debugfs_remove_recursive(hw->debugfs);
	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	devm_free_irq(hw->dev, hw->irq, hw);
	if (ums9117_jpeg_hw_failed(hw))
		return;
	ums9117_jpeg_free_buffers(hw);
	jpeg_restore_qos(hw);
	jpeg_restore_clock_gate(hw);
}
