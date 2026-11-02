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

const struct ums9117_jpeg_scale_geometry
	ums9117_jpeg_scale_geometries[UMS9117_JPEG_SCALE_PROFILE_COUNT] = {
	[UMS9117_JPEG_SCALE_640X480_TO_320X240] = {
		.source = { 640, 480 },
		.destination = { 320, 240 },
	},
	[UMS9117_JPEG_SCALE_320X240_TO_160X120] = {
		.source = { 320, 240 },
		.destination = { 160, 120 },
	},
};

/* Fixed 640x480 -> 320x240 table recovered identically from two stock traces. */
static const u32 jpeg_scale_h[JPEG_SCALE_H_WORDS] = {
	0x0003f600, 0x000e2300, 0x000008c7, 0x00000fd8, 0x0003f400, 0x000e2881,
	0x000f4747, 0x00000fe7, 0x000ff200, 0x000a2e02, 0x000e85e7, 0x00000fef,
	0x000bf1ff, 0x00053304, 0x000e44a7, 0x00000ff7, 0x000ff1ff, 0x000e3706,
	0x000e0366, 0x00000fff, 0x0007f3fe, 0x00063a89, 0x000e0246, 0x00000fff,
	0x000ff5fd, 0x000b3d8b, 0x000e4165, 0x00000007, 0x000bfbfc, 0x00013f0e,
	0x000e8085, 0x00000007, 0x00006800, 0x00001a26, 0x00088a00, 0x000012a5,
	0x000cac02, 0x00000ca3, 0x0000ce05, 0x00000821, 0x000cee09, 0x0000049d,
	0x000d0810, 0x00000299, 0x00091e19, 0x00000115, 0x00052c25, 0x00000011,
};

static const u32 jpeg_scale_v[JPEG_SCALE_V_WORDS] = {
	0x00006834, 0x00013098, 0x00006834, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00004a25, 0x00012c96, 0x00008a45, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00003219, 0x00011e8f,
	0x0000ac56, 0x00000402, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00002010, 0x00010884, 0x0000ce67, 0x00000a05, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00001209, 0x0000ee77, 0x0000ee77, 0x00001209,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000a05, 0x0000ce67,
	0x00010884, 0x00002010, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000402, 0x0000ac56, 0x00011e8f, 0x00003219, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00008a45, 0x00012c96, 0x00004a25,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x000198cc, 0x00000000,
	0x00000000, 0x00006834,
};

static void jpeg_scale_t4_copy(u8 *destination, const u8 *source, size_t size)
{
	size_t i;

	for (i = 0; i < size; ++i)
		destination[i] = source[i ^ 3];
}

static void jpeg_scale_fill_inputs(struct ums9117_jpeg_hw *hw,
				   size_t source_bytes, size_t dest_bytes,
				   const u8 *input[2])
{
	unsigned int plane;

	for (plane = 0; plane < 2; ++plane)
		jpeg_scale_t4_copy(
			ums9117_jpeg_buffer_data(&hw->buffers[plane]),
			input[plane], source_bytes);
	for (plane = 2; plane < 4; ++plane)
		memset(ums9117_jpeg_buffer_data(&hw->buffers[plane]),
		       JPEG_ENCODE_FILL, dest_bytes);
}

static int jpeg_scale_verify_memory(struct ums9117_jpeg_hw *hw,
				    size_t source_bytes, const u8 *input[2])
{
	unsigned int plane;
	size_t i;

	if (!ums9117_jpeg_guards_ok(hw))
		return -EFAULT;
	dma_rmb();
	for (plane = 0; plane < 2; ++plane) {
		const u8 *data = ums9117_jpeg_buffer_data(&hw->buffers[plane]);

		for (i = 0; i < source_bytes; ++i)
			if (data[i] != input[plane][i ^ 3])
				return -EBADMSG;
	}
	return 0;
}

static int jpeg_scale_owner(struct ums9117_jpeg_hw *hw, bool cpu,
			    unsigned long deadline)
{
	ums9117_jpeg_write(hw, JPEG_CFG, cpu ? 0x12 : 0x02);
	return ums9117_jpeg_operation_poll(hw, JPEG_CFG, JPEG_CFG_CPU_ACK,
					   cpu ? JPEG_CFG_CPU_ACK : 0,
					   JPEG_SCALE_IRQ_ERRORS, 0,
					   ums9117_jpeg_subdeadline(deadline));
}

static int jpeg_scale_upload_tables(struct ums9117_jpeg_hw *hw)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(jpeg_scale_h); ++i) {
		ums9117_jpeg_write(hw, JPEG_SCALER_H_TABLE + i * 4,
				   jpeg_scale_h[i]);
		if (ums9117_jpeg_read(hw, JPEG_SCALER_H_TABLE + i * 4) !=
		    jpeg_scale_h[i])
			return -EIO;
	}
	for (i = 0; i < ARRAY_SIZE(jpeg_scale_v); ++i) {
		ums9117_jpeg_write(hw, JPEG_SCALER_V_TABLE + i * 4,
				   jpeg_scale_v[i]);
		if (ums9117_jpeg_read(hw, JPEG_SCALER_V_TABLE + i * 4) !=
		    jpeg_scale_v[i])
			return -EIO;
	}
	return 0;
}

static int
jpeg_scale_configure(struct ums9117_jpeg_hw *hw,
		     const struct ums9117_jpeg_scale_geometry *geometry,
		     unsigned long deadline)
{
	unsigned long flags;
	int ret;

	ums9117_jpeg_reset_submission(hw);
	/* Capture leaves shared DCAM state outside the JPEG reset domain. */
	writel(JPEG_DCAM_RESET, hw->reset_set);
	writel(JPEG_DCAM_RESET, hw->reset_clear);
	spin_lock_irqsave(&hw->lock, flags);
	hw->stats.operation[JPEG_OPERATION_SCALE].resets++;
	spin_unlock_irqrestore(&hw->lock, flags);
	ret = jpeg_scale_owner(hw, true, deadline);
	if (ret)
		return ret;
	ret = jpeg_scale_upload_tables(hw);
	if (ret)
		return ret;
	ret = jpeg_scale_owner(hw, false, deadline);
	if (ret)
		return ret;
	ums9117_jpeg_write(hw, JPEG_SC_FORMAT, 0);
	ums9117_jpeg_write(hw, JPEG_SOURCE_SIZE,
			   (geometry->source.height << 16) |
				   geometry->source.width);
	ums9117_jpeg_write(hw, JPEG_DEST_SIZE,
			   (geometry->destination.height << 16) |
				   geometry->destination.width);
	ums9117_jpeg_write(
		hw, JPEG_FRAME0_Y,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[0])));
	ums9117_jpeg_write(
		hw, JPEG_FRAME0_UV,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[1])));
	ums9117_jpeg_write(hw, JPEG_FRAME1_Y, 0);
	ums9117_jpeg_write(hw, JPEG_FRAME1_UV, 0);
	ums9117_jpeg_write(
		hw, JPEG_STREAM0,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[2])));
	ums9117_jpeg_write(
		hw, JPEG_STREAM1,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[3])));
	ums9117_jpeg_write(hw, JPEG_FRAME6, 0);
	ums9117_jpeg_write(hw, JPEG_BURST_GAP, 0);
	ums9117_jpeg_write(hw, JPEG_ENDIAN, 5);
	ums9117_jpeg_write(hw, JPEG_TRIM_START, 0);
	ums9117_jpeg_write(hw, JPEG_TRIM_SIZE,
			   (geometry->source.height << 16) |
				   geometry->source.width);
	ums9117_jpeg_write(hw, JPEG_SLICE_SIZE, 0);
	ums9117_jpeg_write(hw, JPEG_SCALING_CONFIG, 0x200);
	return 0;
}

static int jpeg_scale_wait(struct ums9117_jpeg_hw *hw, unsigned long deadline)
{
	unsigned long now;
	int ret;

	dma_wmb();
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	ums9117_jpeg_write(hw, JPEG_INT_MASK, JPEG_SCALE_IRQ_ENABLED);
	ums9117_jpeg_read(hw, JPEG_INT_MASK);
	ums9117_jpeg_write(hw, JPEG_SCALING_CONFIG, 0x201);
	now = jiffies;
	if (time_after_eq(now, deadline) ||
	    !wait_for_completion_timeout(&hw->completion, deadline - now))
		return -ETIMEDOUT;
	ret = ums9117_jpeg_operation_check(hw, JPEG_SCALE_IRQ_ERRORS, 0,
					   deadline);
	return ret;
}

static int jpeg_scale_finish(struct ums9117_jpeg_hw *hw, unsigned long deadline)
{
	struct ums9117_jpeg_hw_operation_stats *stats;
	unsigned long flags;
	u32 events, irq_count;
	int ret;

	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	spin_lock_irqsave(&hw->lock, flags);
	events = hw->events | (hw->irq_raw & JPEG_SCALE_IRQ_ENABLED);
	irq_count = hw->active_irq_count;
	spin_unlock_irqrestore(&hw->lock, flags);
	if (!irq_count || !(ums9117_jpeg_events(hw) & JPEG_SCALE_IRQ_DONE) ||
	    (events & JPEG_SCALE_IRQ_ENABLED) != JPEG_SCALE_IRQ_DONE)
		return -EIO;
	ret = ums9117_jpeg_operation_poll(hw, JPEG_SCALING_CONFIG, BIT(0), 0,
					  JPEG_SCALE_IRQ_ERRORS, 0,
					  ums9117_jpeg_subdeadline(deadline));
	if (ret)
		return ret;
	ret = ums9117_jpeg_operation_poll(hw, JPEG_AHB_STATUS, JPEG_AHB_BUSY, 0,
					  JPEG_SCALE_IRQ_ERRORS, 0,
					  ums9117_jpeg_subdeadline(deadline));
	if (ret)
		return ret;
	spin_lock_irqsave(&hw->lock, flags);
	stats = &hw->stats.operation[JPEG_OPERATION_SCALE];
	stats->last_command = ums9117_jpeg_read(hw, JPEG_SCALING_CONFIG);
	spin_unlock_irqrestore(&hw->lock, flags);
	ums9117_jpeg_write(hw, JPEG_CFG, 0);
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	ums9117_jpeg_clear_operation(hw);
	return 0;
}

static int jpeg_scale_recover(struct ums9117_jpeg_hw *hw)
{
	struct ums9117_jpeg_hw_operation_stats *stats;
	unsigned long flags;
	u32 status;
	int ret;

	ums9117_jpeg_write(hw, JPEG_INT_MASK, 0);
	ums9117_jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	writel(JPEG_DCAM_RESET, hw->reset_set);
	writel(JPEG_DCAM_RESET, hw->reset_clear);
	ret = readl_poll_timeout(hw->base + JPEG_AHB_STATUS, status,
				 !(status & JPEG_AHB_BUSY), 10, JPEG_POLL_US);
	spin_lock_irqsave(&hw->lock, flags);
	stats = &hw->stats.operation[JPEG_OPERATION_SCALE];
	stats->resets++;
	stats->recovery_ahb = status;
	stats->last_command = ums9117_jpeg_read(hw, JPEG_SCALING_CONFIG);
	if (ret) {
		stats->idle_failures++;
		WRITE_ONCE(hw->failed, true);
	}
	spin_unlock_irqrestore(&hw->lock, flags);
	ums9117_jpeg_clear_operation(hw);
	if (!ret) {
		ums9117_jpeg_write(hw, JPEG_CFG, 0);
		ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	}
	return ret;
}

int ums9117_jpeg_hw_scale(struct ums9117_jpeg_hw *hw,
			  enum ums9117_jpeg_scale_profile profile,
			  const u8 *input[2], const size_t input_size[2],
			  u8 *output[2], const size_t capacity[2])
{
	const struct ums9117_jpeg_scale_geometry *geometry;
	unsigned long flags, deadline;
	u64 started_ns;
	unsigned int completed_units = 0;
	size_t buffer_sizes[4];
	size_t source_bytes, dest_bytes;
	int memory_ret, recovery_ret, ret;

	if (!input || !input[0] || !input[1] || !input_size || !output ||
	    !output[0] || !output[1] || !capacity)
		return -EINVAL;
	if ((unsigned int)profile >= UMS9117_JPEG_SCALE_PROFILE_COUNT)
		return -EINVAL;
	geometry = &ums9117_jpeg_scale_geometries[profile];
	source_bytes = geometry->source.width * geometry->source.height;
	dest_bytes = geometry->destination.width * geometry->destination.height;
	if (ums9117_jpeg_hw_failed(hw))
		return -EIO;
	if (atomic_read(&hw->cancelled))
		return -ECANCELED;
	if (input_size[0] != source_bytes || input_size[1] != source_bytes)
		return -EINVAL;
	if (capacity[0] < dest_bytes || capacity[1] < dest_bytes)
		return -ENOSPC;
	started_ns = ktime_get_ns();
	ums9117_jpeg_operation_request(hw, JPEG_OPERATION_SCALE);
	buffer_sizes[0] = source_bytes;
	buffer_sizes[1] = source_bytes;
	buffer_sizes[2] = dest_bytes;
	buffer_sizes[3] = dest_bytes;
	ret = ums9117_jpeg_prepare_buffers(hw, buffer_sizes);
	if (ret)
		goto free_buffers;
	if (!ums9117_jpeg_buffers_disjoint(hw)) {
		ret = -ERANGE;
		goto free_buffers;
	}
	jpeg_scale_fill_inputs(hw, source_bytes, dest_bytes, input);
	ums9117_jpeg_set_operation(hw, JPEG_OPERATION_SCALE,
				   JPEG_SCALE_IRQ_ENABLED);
	deadline = jiffies + msecs_to_jiffies(JPEG_WAIT_MS);
	ret = jpeg_scale_configure(hw, geometry, deadline);
	if (!ret)
		ret = jpeg_scale_wait(hw, deadline);
	if (!ret) {
		ret = jpeg_scale_finish(hw, deadline);
		if (!ret)
			completed_units = 1;
	}
	if (ret) {
		recovery_ret = jpeg_scale_recover(hw);
		if (recovery_ret) {
			ret = recovery_ret;
			goto record_result;
		}
	}
	memory_ret = jpeg_scale_verify_memory(hw, source_bytes, input);
	if (memory_ret) {
		spin_lock_irqsave(&hw->lock, flags);
		if (memory_ret == -EBADMSG)
			hw->stats.operation[JPEG_OPERATION_SCALE]
				.input_failures++;
		else
			hw->stats.operation[JPEG_OPERATION_SCALE]
				.guard_failures++;
		WRITE_ONCE(hw->failed, true);
		spin_unlock_irqrestore(&hw->lock, flags);
		recovery_ret = jpeg_scale_recover(hw);
		if (recovery_ret) {
			ret = recovery_ret;
			goto record_result;
		}
		if (!ret)
			ret = memory_ret;
	}
	if (!ret && atomic_read(&hw->cancelled))
		ret = -ECANCELED;
	if (!ret) {
		dma_rmb();
		jpeg_scale_t4_copy(output[0],
				   ums9117_jpeg_buffer_data(&hw->buffers[2]),
				   dest_bytes);
		jpeg_scale_t4_copy(output[1],
				   ums9117_jpeg_buffer_data(&hw->buffers[3]),
				   dest_bytes);
	}
free_buffers:
	if (ret)
		ums9117_jpeg_free_buffers(hw);
record_result:
	ums9117_jpeg_clear_operation(hw);
	ums9117_jpeg_operation_result(hw, JPEG_OPERATION_SCALE, ret,
				      completed_units, ret ? 0 : 2 * dest_bytes,
				      started_ns);
	return ret;
}
