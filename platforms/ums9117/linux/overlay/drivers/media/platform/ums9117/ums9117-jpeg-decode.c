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

static int jpeg_tables(struct ums9117_jpeg_hw *hw,
		       const struct ums9117_jpeg_frame *frame)
{
	u32 value;
	unsigned int i;
	int ret;

	ums9117_jpeg_write(hw, JPEG_CFG, JPEG_CFG_VSP | JPEG_CFG_CPU_TABLES);
	ret = readl_poll_timeout(hw->base + JPEG_CFG, value,
				 value & JPEG_CFG_CPU_ACK, 10, JPEG_POLL_US);
	if (ret)
		return ret;
	for (i = 0; i < UMS9117_JPEG_QUANT_WORDS; ++i)
		ums9117_jpeg_write(hw, JPEG_QUANT_TABLE + 4 * i,
				   frame->quant_words[i]);
	for (i = 0; i < UMS9117_JPEG_HUFF_VALUE_WORDS; ++i)
		ums9117_jpeg_write(hw, JPEG_HUFF_VALUE_TABLE + 4 * i,
				   frame->huff_value_words[i]);
	for (i = 0; i < UMS9117_JPEG_HUFF_META_WORDS; ++i)
		ums9117_jpeg_write(hw, JPEG_HUFF_META_TABLE + 4 * i,
				   frame->huff_meta_words[i]);
	ums9117_jpeg_write(hw, JPEG_CFG, JPEG_CFG_VSP);
	return readl_poll_timeout(hw->base + JPEG_CFG, value,
				  !(value & JPEG_CFG_CPU_ACK), 10,
				  JPEG_POLL_US);
}

static int jpeg_configure(struct ums9117_jpeg_hw *hw,
			  const struct ums9117_jpeg_frame *frame, size_t y_size,
			  unsigned int factor)
{
	u32 count = frame->mcu_x * frame->mcu_y;
	u32 status;
	int ret;

	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	ums9117_jpeg_reset(hw);
	ums9117_jpeg_write(hw, JPEG_CFG, JPEG_CFG_VSP | JPEG_CFG_CPU_TABLES);
	/* Only one slot is valid; its staging memory is reused after idle/copy. */
	ums9117_jpeg_write(
		hw, JPEG_FRAME0_Y,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[1])));
	ums9117_jpeg_write(
		hw, JPEG_FRAME0_UV,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[2])));
	ums9117_jpeg_write(
		hw, JPEG_FRAME1_Y,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[1])));
	ums9117_jpeg_write(
		hw, JPEG_FRAME1_UV,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[2])));
	ums9117_jpeg_write(
		hw, JPEG_STREAM0,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[0])));
	ums9117_jpeg_write(
		hw, JPEG_STREAM1,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[0])));
	ums9117_jpeg_write(hw, JPEG_GLOBAL_CFG0, 0x102cb);
	ums9117_jpeg_write(hw, JPEG_TIMER, 0xffff);
	ums9117_jpeg_write(hw, JPEG_BURST_GAP, 0x1ff);
	ums9117_jpeg_write(hw, JPEG_ENDIAN, (y_size >> 2) << 4);
	ums9117_jpeg_write(hw, JPEG_SOURCE_SIZE,
			   ((frame->padded_height / factor) << 16) |
				   (frame->padded_width / factor));
	ums9117_jpeg_write(hw, JPEG_BSM_CFG1, BIT(31));
	ums9117_jpeg_write(hw, JPEG_BSM_CFG0, hw->buffers[0].size / 4);
	ums9117_jpeg_write(hw, JPEG_VLD_MCU_COUNT, count);
	ums9117_jpeg_write(hw, JPEG_DCT_CONFIG,
			   (frame->mcu_format << 9) | 0x102);
	ums9117_jpeg_write(hw, JPEG_DCT_FINISH, 1);
	ums9117_jpeg_write(hw, JPEG_MBIO_CONFIG,
			   factor == 4 ? 0x2102 :
			   factor == 2 ? 0x1102 :
					 2);
	ret = jpeg_tables(hw, frame);
	if (ret || atomic_read(&hw->cancelled))
		return ret ?: -ECANCELED;
	ums9117_jpeg_write(hw, JPEG_GLOBAL_CFG1,
			   (frame->mcu_format << 24) | (frame->mcu_y << 12) |
				   frame->mcu_x);
	dma_wmb();
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	ums9117_jpeg_write(hw, JPEG_INT_MASK, JPEG_DECODE_IRQ_ENABLED);
	ums9117_jpeg_write(hw, JPEG_BSM_CFG0,
			   JPEG_BSM_BUFFER0 | (hw->buffers[0].size / 4));
	ret = readl_poll_timeout(hw->base + JPEG_BSM_DEBUG, status,
				 status & JPEG_BSM_READY, 10, JPEG_POLL_US);
	if (ret || atomic_read(&hw->cancelled))
		return ret ?: -ECANCELED;
	ums9117_jpeg_write(hw, JPEG_RESTART_COUNT, 0);
	ums9117_jpeg_write(hw, JPEG_RESTART_INTERVAL, frame->restart_interval);
	ums9117_jpeg_write(hw, JPEG_DC_Y, 0);
	ums9117_jpeg_write(hw, JPEG_DC_UV, 0);
	return 0;
}

static int jpeg_start_slice(struct ums9117_jpeg_hw *hw, u32 count,
			    unsigned int index)
{
	unsigned long flags;
	u32 events;

	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	reinit_completion(&hw->completion);
	spin_lock_irqsave(&hw->lock, flags);
	hw->events &= ~JPEG_IRQ_DONE;
	events = hw->events;
	spin_unlock_irqrestore(&hw->lock, flags);
	if (atomic_read(&hw->cancelled))
		return -ECANCELED;
	if (events & JPEG_DECODE_IRQ_ERRORS)
		return -EILSEQ;
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_DONE);
	ums9117_jpeg_write(hw, JPEG_INT_MASK, JPEG_DECODE_IRQ_ENABLED);
	dma_wmb();
	ums9117_jpeg_write(hw, JPEG_MBIO_STATUS, BIT(index & 1));
	ums9117_jpeg_write(hw, JPEG_MBIO_MCU_COUNT, count);
	ums9117_jpeg_write(hw, JPEG_MBIO_ARM, 1);
	ums9117_jpeg_write(hw, JPEG_VLD_MCU_COUNT, count);
	ums9117_jpeg_write(hw, JPEG_VLD_CONTROL, 1);
	return 0;
}

static int jpeg_wait_slice(struct ums9117_jpeg_hw *hw,
			   const struct ums9117_jpeg_frame *frame,
			   u32 completed_rows, unsigned long deadline)
{
	unsigned long flags, now, remaining;
	u32 events, status, progress;
	int ret;

	now = jiffies;
	if (time_after_eq(now, deadline))
		return -ETIMEDOUT;
	if (!wait_for_completion_timeout(&hw->completion, deadline - now))
		return -ETIMEDOUT;
	if (atomic_read(&hw->cancelled))
		return -ECANCELED;
	now = jiffies;
	if (time_after_eq(now, deadline))
		return -ETIMEDOUT;
	remaining = min_t(unsigned long, JPEG_POLL_US,
			  jiffies_to_usecs(deadline - now));
	ret = readl_poll_timeout(hw->base + JPEG_AHB_STATUS, status,
				 !(status & JPEG_AHB_BUSY), 10, remaining);
	if (ret)
		return ret;
	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	spin_lock_irqsave(&hw->lock, flags);
	events = hw->events;
	spin_unlock_irqrestore(&hw->lock, flags);
	events |= ums9117_jpeg_read(hw, JPEG_INT_RAW);
	if (events & JPEG_DECODE_IRQ_ERRORS)
		return -EILSEQ;
	if (!(events & JPEG_IRQ_DONE))
		return -EIO;
	if (time_after_eq(jiffies, deadline))
		return -ETIMEDOUT;
	if (completed_rows == frame->padded_height)
		return (ums9117_jpeg_read(hw, JPEG_MBIO_STATUS) &
			JPEG_MBIO_FRAME_DONE) ?
			       0 :
			       -EIO;
	progress = ums9117_jpeg_read(hw, JPEG_MBIO_PROGRESS);
	if ((progress & 0x3ff) + ((progress >> 16) & 0x3ff) * frame->mcu_x <
	    completed_rows / (8 * frame->vertical_subsampling) * frame->mcu_x)
		return -EIO;
	return 0;
}

int ums9117_jpeg_hw_decode(struct ums9117_jpeg_hw *hw,
			   const struct ums9117_jpeg_frame *frame,
			   const u8 *entropy, u8 *output[2],
			   const size_t capacity[2], unsigned int factor)
{
	unsigned long flags, deadline;
	u64 started_ns;
	size_t full_y_size, y_size, uv_size, slice_y_size, slice_uv_size;
	size_t buffer_sizes[4];
	size_t y_offset;
	u32 events, row, rows, slice_rows;
	unsigned int completed_slices = 0;
	unsigned int index = 0;
	int ret, stop_ret;

	if (!frame || !entropy || !output || !output[0] || !output[1] ||
	    !capacity || (factor != 1 && factor != 2 && factor != 4))
		return -EINVAL;
	if (ums9117_jpeg_hw_failed(hw))
		return -EIO;
	if (atomic_read(&hw->cancelled))
		return -ECANCELED;
	if (factor == 4 && (frame->mcu_format != UMS9117_JPEG_MCU_422 ||
			    frame->vertical_subsampling != 1 ||
			    frame->width != frame->padded_width ||
			    frame->height != frame->padded_height))
		return -EINVAL;
	if (!frame->padded_width || !frame->padded_height ||
	    frame->padded_width > UMS9117_JPEG_MAX_DIMENSION ||
	    frame->padded_height > UMS9117_JPEG_MAX_DIMENSION ||
	    (frame->vertical_subsampling != 1 &&
	     frame->vertical_subsampling != 2) ||
	    !frame->entropy_length ||
	    frame->entropy_length > UMS9117_JPEG_MAX_INPUT_SIZE ||
	    check_mul_overflow((size_t)frame->padded_width,
			       (size_t)frame->padded_height, &full_y_size))
		return -EINVAL;
	y_size = full_y_size / (factor * factor);
	uv_size = y_size / frame->vertical_subsampling;
	if (capacity[0] < y_size || capacity[1] < uv_size)
		return -ENOSPC;
	slice_rows = min_t(u32, frame->padded_height, JPEG_SLICE_ROWS);
	slice_y_size =
		(size_t)(frame->padded_width / factor) * (slice_rows / factor);
	slice_uv_size = slice_y_size / frame->vertical_subsampling;

	started_ns = ktime_get_ns();
	ums9117_jpeg_operation_request(hw, JPEG_OPERATION_DECODE);
	buffer_sizes[0] = frame->entropy_length + JPEG_STREAM_TAIL_BYTES;
	buffer_sizes[1] = slice_y_size;
	buffer_sizes[2] = slice_uv_size;
	buffer_sizes[3] = 0;
	ret = ums9117_jpeg_prepare_buffers(hw, buffer_sizes);
	if (ret)
		goto free_buffers;
	memcpy(ums9117_jpeg_buffer_data(&hw->buffers[0]), entropy,
	       frame->entropy_length);
	ums9117_jpeg_set_operation(hw, JPEG_OPERATION_DECODE,
				   JPEG_DECODE_IRQ_ENABLED);
	deadline = jiffies + msecs_to_jiffies(JPEG_WAIT_MS);
	ret = jpeg_configure(hw, frame, y_size, factor);
	if (ret)
		goto stop;
	for (row = 0, index = 0; row < frame->padded_height;
	     row += rows, ++index) {
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			goto stop;
		}
		rows = min(slice_rows, frame->padded_height - row);
		ret = jpeg_start_slice(
			hw,
			rows / (8 * frame->vertical_subsampling) * frame->mcu_x,
			index);
		if (ret)
			goto stop;
		ret = jpeg_wait_slice(hw, frame, row + rows, deadline);
		if (ret)
			goto stop;
		if (!ums9117_jpeg_guards_ok(hw)) {
			ret = -EIO;
			goto stop;
		}
		if (atomic_read(&hw->cancelled)) {
			ret = -ECANCELED;
			goto stop;
		}
		slice_y_size = (size_t)(frame->padded_width / factor) *
			       (rows / factor);
		slice_uv_size = slice_y_size / frame->vertical_subsampling;
		y_offset =
			(size_t)(frame->padded_width / factor) * (row / factor);
		dma_rmb();
		memcpy(output[0] + y_offset,
		       ums9117_jpeg_buffer_data(&hw->buffers[1]), slice_y_size);
		memcpy(output[1] + y_offset / frame->vertical_subsampling,
		       ums9117_jpeg_buffer_data(&hw->buffers[2]),
		       slice_uv_size);
		memset(ums9117_jpeg_buffer_data(&hw->buffers[1]), 0,
		       hw->buffers[1].size);
		memset(ums9117_jpeg_buffer_data(&hw->buffers[2]), 0,
		       hw->buffers[2].size);
		spin_lock_irqsave(&hw->lock, flags);
		hw->stats.slices++;
		spin_unlock_irqrestore(&hw->lock, flags);
		completed_slices++;
	}
stop:
	stop_ret = ums9117_jpeg_stop(hw);
	if (stop_ret) {
		ret = stop_ret;
		goto record_result;
	}
	spin_lock_irqsave(&hw->lock, flags);
	events = hw->stats.last_raw | hw->stats.last_status;
	spin_unlock_irqrestore(&hw->lock, flags);
	if (!ret && (events & JPEG_DECODE_IRQ_ERRORS))
		ret = -EILSEQ;
	if (!ums9117_jpeg_guards_ok(hw)) {
		spin_lock_irqsave(&hw->lock, flags);
		hw->stats.operation[JPEG_OPERATION_DECODE].guard_failures++;
		WRITE_ONCE(hw->failed, true);
		spin_unlock_irqrestore(&hw->lock, flags);
		ret = -EIO;
	}
	if (memcmp(ums9117_jpeg_buffer_data(&hw->buffers[0]), entropy,
		   frame->entropy_length)) {
		spin_lock_irqsave(&hw->lock, flags);
		hw->stats.operation[JPEG_OPERATION_DECODE].input_failures++;
		WRITE_ONCE(hw->failed, true);
		spin_unlock_irqrestore(&hw->lock, flags);
		ret = -EIO;
	}
	if (!ret && atomic_read(&hw->cancelled))
		ret = -ECANCELED;
free_buffers:
	if (ret)
		ums9117_jpeg_free_buffers(hw);
record_result:
	ums9117_jpeg_clear_operation(hw);
	spin_lock_irqsave(&hw->lock, flags);
	hw->stats.last_result = ret;
	spin_unlock_irqrestore(&hw->lock, flags);
	ums9117_jpeg_operation_result(hw, JPEG_OPERATION_DECODE, ret,
				      completed_slices, y_size + uv_size,
				      started_ns);
	return ret;
}
