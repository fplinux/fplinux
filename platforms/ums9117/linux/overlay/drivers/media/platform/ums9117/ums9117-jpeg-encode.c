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

static int jpeg_encode_owner(struct ums9117_jpeg_hw *hw, bool cpu,
			     unsigned long deadline)
{
	ums9117_jpeg_write(hw, JPEG_CFG, cpu ? 0x19 : 0x09);
	return ums9117_jpeg_operation_poll(hw, JPEG_CFG, JPEG_CFG_CPU_ACK,
					   cpu ? JPEG_CFG_CPU_ACK : 0,
					   JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
					   ums9117_jpeg_subdeadline(deadline));
}

static int
jpeg_encode_configure(struct ums9117_jpeg_hw *hw,
		      const struct ums9117_jpeg_encode_config *config,
		      size_t strip_plane_size, size_t stream_size,
		      unsigned long deadline)
{
	u32 count = config->mcu_x * config->mcu_y;
	unsigned int i;
	int ret;

	ums9117_jpeg_reset_submission(hw);
	ums9117_jpeg_reset(hw);
	ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	ret = jpeg_encode_owner(hw, true, deadline);
	if (ret)
		return ret;
	ums9117_jpeg_write(hw, JPEG_SOURCE_SIZE,
			   (config->height << 16) | config->width);
	ums9117_jpeg_write(
		hw, JPEG_FRAME0_Y,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[0])));
	ums9117_jpeg_write(
		hw, JPEG_FRAME0_UV,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[0])) +
			strip_plane_size);
	ums9117_jpeg_write(
		hw, JPEG_FRAME1_Y,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[1])));
	ums9117_jpeg_write(
		hw, JPEG_FRAME1_UV,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[1])) +
			strip_plane_size);
	ums9117_jpeg_write(
		hw, JPEG_STREAM0,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[2])));
	ums9117_jpeg_write(
		hw, JPEG_STREAM1,
		lower_32_bits(ums9117_jpeg_buffer_dma(&hw->buffers[3])));
	ums9117_jpeg_write(hw, JPEG_ENDIAN, 4);
	ums9117_jpeg_write(hw, JPEG_GLOBAL_CFG0, 0x112b3);
	ums9117_jpeg_write(hw, JPEG_GLOBAL_CFG1,
			   (UMS9117_JPEG_MCU_422 << 24) |
				   (config->mcu_y << 12) | config->mcu_x);
	ums9117_jpeg_write(hw, JPEG_TIMER, 0xffff);
	ums9117_jpeg_write(hw, JPEG_BURST_GAP, 0x1ff);
	ums9117_jpeg_write(hw, JPEG_ENDIAN,
			   (((config->width * config->height) / 4) << 4) | 4);
	ums9117_jpeg_write(hw, JPEG_BSM_CFG0, stream_size / 4);
	ums9117_jpeg_write(hw, JPEG_VLC_MCU_COUNT, count);
	ums9117_jpeg_write(hw, JPEG_DCT_CONFIG, 0x103);
	ums9117_jpeg_write(hw, JPEG_DCT_FINISH, 1);
	ums9117_jpeg_write(hw, JPEG_MBIO_CONFIG, 2);
	for (i = 0; i < UMS9117_JPEG_ENCODE_AC_LUT_WORDS; ++i)
		ums9117_jpeg_write(hw, JPEG_HUFF_VALUE_TABLE + i * 4,
				   config->ac_lut_words[i]);
	ret = jpeg_encode_owner(hw, false, deadline);
	if (ret)
		return ret;
	ret = jpeg_encode_owner(hw, true, deadline);
	if (ret)
		return ret;
	for (i = 0; i < UMS9117_JPEG_ENCODE_QBUF_WORDS; ++i)
		ums9117_jpeg_write(hw, JPEG_QUANT_TABLE + i * 4,
				   config->qbuf_words[i]);
	ret = jpeg_encode_owner(hw, false, deadline);
	if (!ret) {
		ums9117_jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
		ums9117_jpeg_write(hw, JPEG_INT_MASK, JPEG_ENCODE_IRQ_ENABLED);
		ums9117_jpeg_read(hw, JPEG_INT_MASK);
	}
	return ret;
}

static int jpeg_encode_bsm_write(struct ums9117_jpeg_hw *hw, u32 bits,
				 u32 value, unsigned long deadline)
{
	int ret;

	ret = ums9117_jpeg_operation_poll(hw, JPEG_BSM_WRITE_READY, BIT(0),
					  BIT(0), JPEG_ENCODE_IRQ_ERRORS,
					  JPEG_IRQ_BSM, deadline);
	if (ret)
		return ret;
	ums9117_jpeg_write(hw, JPEG_BSM_CONTROL, bits << 24);
	ums9117_jpeg_write(hw, JPEG_BSM_DATA, value);
	return 0;
}

static int jpeg_encode_wait_done(struct ums9117_jpeg_hw *hw,
				 unsigned long deadline)
{
	const u32 done = JPEG_IRQ_DONE | JPEG_IRQ_VLC;

	for (;;) {
		unsigned long now;
		u32 events;
		int ret;

		reinit_completion(&hw->completion);
		ret = ums9117_jpeg_operation_check(hw, JPEG_ENCODE_IRQ_ERRORS,
						   JPEG_IRQ_BSM, deadline);
		if (ret) {
			if (ret == -ETIMEDOUT)
				ums9117_jpeg_encode_snapshot_timeout(hw, false,
								     0, 0, 0);
			return ret;
		}
		events = ums9117_jpeg_events(hw);
		if ((events & done) == done)
			return 0;
		now = jiffies;
		if (time_after_eq(now, deadline) ||
		    !wait_for_completion_timeout(&hw->completion,
						 deadline - now)) {
			ums9117_jpeg_encode_snapshot_timeout(hw, false, 0, 0,
							     0);
			return -ETIMEDOUT;
		}
	}
}

static void jpeg_encode_fill_inputs(struct ums9117_jpeg_hw *hw,
				    const u8 *input[2], size_t offset,
				    size_t strip_plane_size)
{
	unsigned int bank;

	for (bank = 0; bank < 2; ++bank) {
		u8 *data = ums9117_jpeg_buffer_data(&hw->buffers[bank]);

		memcpy(data, input[0] + offset, strip_plane_size);
		memcpy(data + strip_plane_size, input[1] + offset,
		       strip_plane_size);
	}
	memset(ums9117_jpeg_buffer_data(&hw->buffers[2]), JPEG_ENCODE_FILL,
	       hw->buffers[2].size);
}

static int jpeg_encode_verify_memory(struct ums9117_jpeg_hw *hw,
				     const u8 *input[2], size_t offset,
				     size_t strip_plane_size,
				     size_t stream_size)
{
	unsigned int bank;

	if (!ums9117_jpeg_guards_ok(hw))
		return -EFAULT;
	for (bank = 0; bank < 2; ++bank) {
		u8 *data = ums9117_jpeg_buffer_data(&hw->buffers[bank]);

		if (memcmp(data, input[0] + offset, strip_plane_size) ||
		    memcmp(data + strip_plane_size, input[1] + offset,
			   strip_plane_size))
			return -EBADMSG;
	}
	for (bank = 2; bank < 4; ++bank) {
		u8 *data = ums9117_jpeg_buffer_data(&hw->buffers[bank]);

		if (memchr_inv(data + stream_size, JPEG_ENCODE_FILL,
			       hw->buffers[bank].size - stream_size))
			return -EFAULT;
	}
	return 0;
}

static int jpeg_encode_strip(struct ums9117_jpeg_hw *hw,
			     const struct ums9117_jpeg_encode_config *config,
			     const u8 *input[2], size_t input_offset,
			     size_t strip_plane_size, size_t stream_size,
			     unsigned int strip, unsigned int strips,
			     u8 *output, size_t capacity, size_t *result_size,
			     unsigned long deadline)
{
	u32 bits;
	u32 count = config->mcu_x * (UMS9117_JPEG_ENCODE_STRIP_ROWS / 8);
	unsigned int i;
	int ret;

	if (stream_size < 4)
		return -ENOSPC;
	jpeg_encode_fill_inputs(hw, input, input_offset, strip_plane_size);
	ret = jpeg_encode_configure(hw, config, strip_plane_size, stream_size,
				    deadline);
	if (ret)
		return ret;
	if (!strip) {
		for (i = 0; i < UMS9117_JPEG_ENCODE_HEADER_SIZE; ++i) {
			ret = jpeg_encode_bsm_write(hw, 8, config->header[i],
						    deadline);
			if (ret)
				return ret;
		}
	}
	dma_wmb();
	ums9117_jpeg_write(hw, JPEG_ENDIAN, ((strip_plane_size / 4) << 4) | 4);
	ums9117_jpeg_write(hw, JPEG_VLC_MCU_COUNT, count);
	ums9117_jpeg_write(hw, JPEG_MBIO_MCU_COUNT, count);
	ums9117_jpeg_write(hw, JPEG_MBIO_STATUS, 1);
	ums9117_jpeg_write(hw, JPEG_MBIO_ARM, 1);
	ret = jpeg_encode_wait_done(hw, deadline);
	if (ret)
		return ret;
	/* MBIO completion releases the input, not pending AHB transfers. */
	ret = ums9117_jpeg_operation_poll(hw, JPEG_AHB_STATUS, JPEG_AHB_BUSY, 0,
					  JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
					  deadline);
	if (ret)
		return ret;
	if (strip + 1 == strips) {
		ret = ums9117_jpeg_operation_poll(hw, JPEG_VLC_CONTROL, BIT(31),
						  0, JPEG_ENCODE_IRQ_ERRORS,
						  JPEG_IRQ_BSM, deadline);
		if (ret)
			return ret;
	}
	ums9117_jpeg_write(hw, JPEG_VLC_CONTROL, 1);
	if (strip + 1 < strips) {
		ret = jpeg_encode_bsm_write(hw, 8, 0xff, deadline);
		if (!ret)
			ret = jpeg_encode_bsm_write(hw, 8, 0xd0 + (strip & 7),
						    deadline);
	} else {
		ret = jpeg_encode_bsm_write(hw, 16, 0xffd9, deadline);
		if (!ret)
			ums9117_jpeg_write(hw, JPEG_VLC_CONTROL, 1);
	}
	if (ret)
		return ret;
	ret = ums9117_jpeg_operation_poll(hw, JPEG_BSM_WRITE_READY, BIT(0),
					  BIT(0), JPEG_ENCODE_IRQ_ERRORS,
					  JPEG_IRQ_BSM, deadline);
	if (ret)
		return ret;
	ums9117_jpeg_write(hw, JPEG_BSM_CONTROL, 2);
	ret = ums9117_jpeg_operation_poll(hw, JPEG_BSM_DEBUG, JPEG_BSM_READY,
					  JPEG_BSM_READY,
					  JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
					  deadline);
	if (ret)
		return ret;
	bits = ums9117_jpeg_read(hw, JPEG_BSM_BITS);
	if (!bits || (bits & 7) || bits / 8 >= stream_size)
		return -EOVERFLOW;
	ret = ums9117_jpeg_operation_poll(hw, JPEG_AHB_STATUS, JPEG_AHB_BUSY, 0,
					  JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
					  deadline);
	if (ret)
		return ret;
	ret = jpeg_encode_verify_memory(hw, input, input_offset,
					strip_plane_size, stream_size);
	if (ret)
		return ret;
	if (!memchr_inv(ums9117_jpeg_buffer_data(&hw->buffers[2]),
			JPEG_ENCODE_FILL, bits / 8))
		return -ENODATA;
	if (bits / 8 > capacity - *result_size)
		return -ENOSPC;
	dma_rmb();
	memcpy(output + *result_size, ums9117_jpeg_buffer_data(&hw->buffers[2]),
	       bits / 8);
	*result_size += bits / 8;
	return 0;
}

int ums9117_jpeg_hw_encode(struct ums9117_jpeg_hw *hw,
			   const struct ums9117_jpeg_encode_config *config,
			   const u8 *input[2], const size_t input_size[2],
			   u8 *output, size_t capacity, size_t *written)
{
	unsigned long flags, deadline;
	u64 started_ns;
	size_t input_plane_size, staged_stream_size = 0;
	size_t strip_plane_size, stream_capacity, result_size = 0;
	size_t buffer_sizes[4];
	unsigned int completed_strips = 0;
	unsigned int staged_strip = 0;
	unsigned int strip, strips;
	int memory_ret, ret, stop_ret;

	if (!config || !input || !input[0] || !input[1] || !input_size ||
	    !output || !written)
		return -EINVAL;
	*written = 0;
	if (ums9117_jpeg_hw_failed(hw))
		return -EIO;
	if (atomic_read(&hw->cancelled))
		return -ECANCELED;
	if (!config->width || !config->height ||
	    config->width > UMS9117_JPEG_MAX_DIMENSION ||
	    config->height > UMS9117_JPEG_MAX_DIMENSION ||
	    !IS_ALIGNED(config->width, 16) ||
	    !IS_ALIGNED(config->height, UMS9117_JPEG_ENCODE_STRIP_ROWS) ||
	    config->mcu_x != config->width / 16 ||
	    config->mcu_y != config->height / 8 ||
	    config->restart_interval !=
		    config->mcu_x * (UMS9117_JPEG_ENCODE_STRIP_ROWS / 8) ||
	    check_mul_overflow((size_t)config->width, (size_t)config->height,
			       &input_plane_size))
		return -EINVAL;
	strip_plane_size =
		(size_t)config->width * UMS9117_JPEG_ENCODE_STRIP_ROWS;
	if (input_size[0] != input_plane_size ||
	    input_size[1] != input_plane_size)
		return -EINVAL;
	if (capacity < UMS9117_JPEG_ENCODE_HEADER_SIZE + 2 ||
	    capacity > JPEG_ENCODE_MAX_OUTPUT_SIZE)
		return -ENOSPC;
	/*
	 * Only one 16-row strip occupies a stream bank. Each 4:2:2 MCU has
	 * four blocks. Bound each block by 64 pairs of 16-bit Huffman codes
	 * and 16-bit amplitudes, doubled for byte stuffing: 512 bytes.
	 * Reserve the header and eight bytes for padding, marker and flush.
	 */
	stream_capacity = (size_t)config->mcu_x *
				  (UMS9117_JPEG_ENCODE_STRIP_ROWS / 8) * 4 *
				  (UMS9117_JPEG_BLOCK_COEFFICIENTS * 8) +
			  UMS9117_JPEG_ENCODE_HEADER_SIZE + 8;
	stream_capacity =
		min(ALIGN(stream_capacity, 4), round_down(capacity, (size_t)4));
	strips = config->height / UMS9117_JPEG_ENCODE_STRIP_ROWS;
	started_ns = ktime_get_ns();
	ums9117_jpeg_operation_request(hw, JPEG_OPERATION_ENCODE);
	buffer_sizes[0] = 2 * strip_plane_size;
	buffer_sizes[1] = 2 * strip_plane_size;
	buffer_sizes[2] = stream_capacity;
	buffer_sizes[3] = stream_capacity;
	ret = ums9117_jpeg_prepare_buffers(hw, buffer_sizes);
	if (ret)
		goto free_buffers;
	if (!ums9117_jpeg_buffers_disjoint(hw)) {
		ret = -ERANGE;
		goto free_buffers;
	}
	memset(ums9117_jpeg_buffer_data(&hw->buffers[3]), JPEG_ENCODE_FILL,
	       hw->buffers[3].size);
	ums9117_jpeg_set_operation(hw, JPEG_OPERATION_ENCODE,
				   JPEG_ENCODE_IRQ_ENABLED);
	deadline = jiffies + msecs_to_jiffies(JPEG_WAIT_MS);
	for (strip = 0; strip < strips; ++strip) {
		size_t stream_size = round_down(min(capacity - result_size,
						    hw->buffers[2].size),
						(size_t)4);

		if (stream_size < 4) {
			ret = -ENOSPC;
			break;
		}
		WRITE_ONCE(hw->active_encode_strip, strip);
		staged_strip = strip;
		staged_stream_size = stream_size;
		ret = jpeg_encode_strip(hw, config, input,
					(size_t)strip * strip_plane_size,
					strip_plane_size, stream_size, strip,
					strips, output, capacity, &result_size,
					deadline);
		if (ret)
			break;
		completed_strips++;
	}
	stop_ret = ums9117_jpeg_stop(hw);
	if (stop_ret) {
		ret = stop_ret;
		goto record_result;
	}
	memory_ret = jpeg_encode_verify_memory(
		hw, input, (size_t)staged_strip * strip_plane_size,
		strip_plane_size, staged_stream_size);
	if (!memory_ret && !ret &&
	    memchr_inv(ums9117_jpeg_buffer_data(&hw->buffers[3]),
		       JPEG_ENCODE_FILL, hw->buffers[3].size))
		memory_ret = -EFAULT;
	if (memory_ret) {
		spin_lock_irqsave(&hw->lock, flags);
		if (memory_ret == -EBADMSG)
			hw->stats.operation[JPEG_OPERATION_ENCODE]
				.input_failures++;
		else
			hw->stats.operation[JPEG_OPERATION_ENCODE]
				.guard_failures++;
		WRITE_ONCE(hw->failed, true);
		spin_unlock_irqrestore(&hw->lock, flags);
		ret = memory_ret;
	}
	if (ret == -ENODATA) {
		spin_lock_irqsave(&hw->lock, flags);
		hw->stats.operation[JPEG_OPERATION_ENCODE].output_failures++;
		WRITE_ONCE(hw->failed, true);
		spin_unlock_irqrestore(&hw->lock, flags);
	}
	if (!ret && atomic_read(&hw->cancelled))
		ret = -ECANCELED;
	if (!ret)
		*written = result_size;
free_buffers:
	if (ret)
		ums9117_jpeg_free_buffers(hw);
record_result:
	ums9117_jpeg_clear_operation(hw);
	ums9117_jpeg_operation_result(hw, JPEG_OPERATION_ENCODE, ret,
				      completed_strips, result_size,
				      started_ns);
	return ret;
}
