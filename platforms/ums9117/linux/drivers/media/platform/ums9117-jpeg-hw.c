// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
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
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "ums9117-jpeg-codec.h"
#include "ums9117-jpeg-hw.h"

#define JPEG_MMIO_BYTES 0x11a00U
#define JPEG_GATE BIT(10)
#define JPEG_DCAM_RESET BIT(0)
#define JPEG_RESET BIT(8)
#define JPEG_CLOCK_MASK (GENMASK(9, 8) | GENMASK(1, 0))

#define JPEG_CFG 0x00000U
#define JPEG_SC_FORMAT 0x00008U
#define JPEG_SOURCE_SIZE 0x0000cU
#define JPEG_DEST_SIZE 0x00010U
#define JPEG_TIMER 0x00014U
#define JPEG_INT_STATUS 0x00020U
#define JPEG_INT_MASK 0x00024U
#define JPEG_INT_CLEAR 0x00028U
#define JPEG_INT_RAW 0x0002cU
#define JPEG_FRAME0_Y 0x00040U
#define JPEG_FRAME0_UV 0x00044U
#define JPEG_FRAME1_Y 0x00048U
#define JPEG_FRAME1_UV 0x0004cU
#define JPEG_STREAM0 0x00050U
#define JPEG_STREAM1 0x00054U
#define JPEG_FRAME6 0x00058U
#define JPEG_BURST_GAP 0x00060U
#define JPEG_ENDIAN 0x00064U
#define JPEG_AHB_STATUS 0x00068U
#define JPEG_QUANT_TABLE 0x00300U
#define JPEG_SCALER_H_TABLE 0x00200U
#define JPEG_SCALER_V_TABLE 0x00300U
#define JPEG_SCALING_CONFIG 0x00130U
#define JPEG_TRIM_START 0x00134U
#define JPEG_TRIM_SIZE 0x00138U
#define JPEG_SLICE_SIZE 0x0013cU
#define JPEG_HUFF_VALUE_TABLE 0x02000U
#define JPEG_GLOBAL_CFG0 0x10000U
#define JPEG_GLOBAL_CFG1 0x10004U
#define JPEG_BSM_CFG0 0x10400U
#define JPEG_BSM_CFG1 0x10404U
#define JPEG_BSM_CONTROL 0x10408U
#define JPEG_BSM_DATA 0x1040cU
#define JPEG_BSM_BITS 0x10414U
#define JPEG_BSM_DEBUG 0x10418U
#define JPEG_BSM_WRITE_READY 0x10420U
#define JPEG_VLD_CONTROL 0x10800U
#define JPEG_RESTART_COUNT 0x10870U
#define JPEG_DC_Y 0x10874U
#define JPEG_DC_UV 0x10878U
#define JPEG_VLD_MCU_COUNT 0x1087cU
#define JPEG_RESTART_INTERVAL 0x10880U
#define JPEG_HUFF_META_TABLE 0x10884U
#define JPEG_VLC_MCU_COUNT 0x10c00U
#define JPEG_VLC_CONTROL 0x10c04U
#define JPEG_DCT_CONFIG 0x11000U
#define JPEG_DCT_FINISH 0x11040U
#define JPEG_MBIO_CONFIG 0x11800U
#define JPEG_MBIO_PROGRESS 0x11808U
#define JPEG_MBIO_MCU_COUNT 0x1180cU
#define JPEG_MBIO_STATUS 0x11810U
#define JPEG_MBIO_ARM 0x11818U

#define JPEG_CFG_VSP (BIT(0) | BIT(3))
#define JPEG_CFG_CPU_TABLES BIT(4)
#define JPEG_CFG_CPU_ACK BIT(7)
#define JPEG_AHB_BUSY BIT(0)
#define JPEG_BSM_BUFFER0 BIT(31)
#define JPEG_BSM_BUFFERS GENMASK(31, 30)
#define JPEG_BSM_READY BIT(31)
#define JPEG_MBIO_FRAME_DONE BIT(2)

#define JPEG_IRQ_BSM BIT(7)
#define JPEG_IRQ_VLC BIT(8)
#define JPEG_IRQ_DONE BIT(9)
#define JPEG_IRQ_TIMEOUT BIT(12)
#define JPEG_IRQ_VLD_ERROR BIT(13)
#define JPEG_DECODE_IRQ_ERRORS (JPEG_IRQ_TIMEOUT | JPEG_IRQ_VLD_ERROR)
#define JPEG_DECODE_IRQ_ENABLED \
	(JPEG_IRQ_BSM | JPEG_IRQ_DONE | JPEG_DECODE_IRQ_ERRORS)
#define JPEG_ENCODE_IRQ_ERRORS (BIT(11) | BIT(12) | BIT(13))
#define JPEG_ENCODE_IRQ_ENABLED \
	(JPEG_IRQ_BSM | JPEG_IRQ_VLC | JPEG_IRQ_DONE | JPEG_ENCODE_IRQ_ERRORS)
#define JPEG_SCALE_IRQ_DONE BIT(4)
#define JPEG_SCALE_IRQ_ERRORS (BIT(5) | BIT(6) | BIT(10))
#define JPEG_SCALE_IRQ_ENABLED (JPEG_SCALE_IRQ_DONE | JPEG_SCALE_IRQ_ERRORS)
#define JPEG_IRQ_KNOWN GENMASK(13, 0)

#define JPEG_WAIT_MS 3000U
#define JPEG_POLL_US 100000U
#define JPEG_GUARD_BYTES 256U
#define JPEG_GUARD_VALUE 0xa5
#define JPEG_STREAM_TAIL_BYTES 64U
/* 2048 pixels * 64 rows plus guards fits a 256 KiB DMA allocation. */
#define JPEG_SLICE_ROWS 64U
#define JPEG_ENCODE_MAX_OUTPUT_SIZE UMS9117_JPEG_MAX_INPUT_SIZE
#define JPEG_ENCODE_FILL 0xcc

#define JPEG_SCALE_H_WORDS 48U
#define JPEG_SCALE_V_WORDS 68U

struct ums9117_jpeg_scale_config {
	u16 source_width;
	u16 source_height;
	u16 dest_width;
	u16 dest_height;
	size_t source_bytes;
	size_t dest_bytes;
};

static const struct ums9117_jpeg_scale_config
	jpeg_scale_configs[UMS9117_JPEG_SCALE_PROFILE_COUNT] = {
	[UMS9117_JPEG_SCALE_640X480_TO_320X240] = {
		.source_width = 640,
		.source_height = 480,
		.dest_width = 320,
		.dest_height = 240,
		.source_bytes = 640U * 480U,
		.dest_bytes = 320U * 240U,
	},
	[UMS9117_JPEG_SCALE_320X240_TO_160X120] = {
		.source_width = 320,
		.source_height = 240,
		.dest_width = 160,
		.dest_height = 120,
		.source_bytes = 320U * 240U,
		.dest_bytes = 160U * 120U,
	},
};

enum ums9117_jpeg_hw_operation {
	JPEG_OPERATION_NONE,
	JPEG_OPERATION_DECODE,
	JPEG_OPERATION_ENCODE,
	JPEG_OPERATION_SCALE,
	JPEG_OPERATION_COUNT,
};

struct ums9117_jpeg_dma_buffer {
	void *allocation;
	dma_addr_t dma;
	size_t allocation_size;
	size_t size;
};

struct ums9117_jpeg_hw_operation_stats {
	u64 requests;
	u64 completed;
	u64 units;
	u64 irqs;
	u64 completion_irqs;
	u64 error_irqs;
	u64 timeouts;
	u64 cancellations;
	u64 resets;
	u64 guard_failures;
	u64 input_failures;
	u64 output_failures;
	u64 idle_failures;
	u64 last_duration_ns;
	u64 last_bytes;
	u32 last_events;
	u32 last_ahb;
	u32 last_command;
	u32 recovery_ahb;
	int last_result;
};

struct ums9117_jpeg_encode_timeout_stats {
	bool valid;
	bool poll_valid;
	u32 strip;
	u32 poll_offset;
	u32 poll_mask;
	u32 poll_expected;
	u32 poll_value;
	u32 irq_events;
	u32 irq_raw;
	u32 bsm_debug;
	u32 bsm_write_ready;
	u32 bsm_bits;
	u32 vlc_control;
	u32 ahb;
};

struct ums9117_jpeg_hw_stats {
	u64 requests;
	u64 completed;
	u64 slices;
	u64 irqs;
	u64 done_irqs;
	u64 bsm_irqs;
	u64 error_irqs;
	u64 timeouts;
	u64 cancellations;
	u64 resets;
	u64 guard_failures;
	u64 input_failures;
	u64 idle_failures;
	u32 last_status;
	u32 last_raw;
	u32 last_mbio;
	u32 last_progress;
	u32 last_ahb;
	u32 last_bsm_cfg0;
	u32 last_bsm_cfg1;
	u32 last_bsm_debug;
	u32 last_vld;
	u32 recovery_ahb;
	int last_result;
	struct ums9117_jpeg_hw_operation_stats operation[JPEG_OPERATION_COUNT];
	struct ums9117_jpeg_encode_timeout_stats encode_timeout;
};

struct ums9117_jpeg_hw {
	struct device *dev;
	void __iomem *base;
	void __iomem *gate_state;
	void __iomem *gate_set;
	void __iomem *gate_clear;
	void __iomem *reset_set;
	void __iomem *reset_clear;
	void __iomem *clock;
	u32 saved_gate;
	u32 saved_clock;
	int irq;
	spinlock_t lock;
	struct completion completion;
	atomic_t cancelled;
	enum ums9117_jpeg_hw_operation operation;
	u32 active_encode_strip;
	u32 active_irq_mask;
	u32 active_irq_count;
	u32 operation_events;
	u32 events;
	u32 irq_raw;
	bool failed;
	struct ums9117_jpeg_dma_buffer buffers[4];
	struct ums9117_jpeg_hw_stats stats;
	struct dentry *debugfs;
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

static u32 jpeg_read(struct ums9117_jpeg_hw *hw, u32 reg)
{
	return readl(hw->base + reg);
}

static void jpeg_write(struct ums9117_jpeg_hw *hw, u32 reg, u32 value)
{
	writel(value, hw->base + reg);
}

static int jpeg_wait_idle(struct ums9117_jpeg_hw *hw, u32 *status)
{
	return readl_poll_timeout(hw->base + JPEG_AHB_STATUS, *status,
				  !(*status & JPEG_AHB_BUSY), 10, JPEG_POLL_US);
}

static void jpeg_snapshot(struct ums9117_jpeg_hw *hw)
{
	unsigned long flags;
	struct ums9117_jpeg_hw_stats snapshot = {
		.last_status = jpeg_read(hw, JPEG_INT_STATUS),
		.last_raw = jpeg_read(hw, JPEG_INT_RAW),
		.last_mbio = jpeg_read(hw, JPEG_MBIO_STATUS),
		.last_progress = jpeg_read(hw, JPEG_MBIO_PROGRESS),
		.last_ahb = jpeg_read(hw, JPEG_AHB_STATUS),
		.last_bsm_cfg0 = jpeg_read(hw, JPEG_BSM_CFG0),
		.last_bsm_cfg1 = jpeg_read(hw, JPEG_BSM_CFG1),
		.last_bsm_debug = jpeg_read(hw, JPEG_BSM_DEBUG),
		.last_vld = jpeg_read(hw, JPEG_VLD_CONTROL),
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
	u32 status = jpeg_read(hw, JPEG_INT_STATUS) & mask;
	u32 raw;
	unsigned long flags;

	if (!status)
		return IRQ_NONE;
	raw = jpeg_read(hw, JPEG_INT_RAW);
	jpeg_write(hw, JPEG_INT_CLEAR, status);

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
	if (operation == JPEG_OPERATION_DECODE) {
		hw->stats.irqs++;
		if (status & JPEG_IRQ_DONE)
			hw->stats.done_irqs++;
		if (status & JPEG_IRQ_BSM)
			hw->stats.bsm_irqs++;
		if (status & JPEG_DECODE_IRQ_ERRORS)
			hw->stats.error_irqs++;
	}
	spin_unlock_irqrestore(&hw->lock, flags);

	if (operation != JPEG_OPERATION_DECODE ||
	    status & (JPEG_IRQ_DONE | JPEG_DECODE_IRQ_ERRORS))
		complete(&hw->completion);
	return IRQ_HANDLED;
}

static void *jpeg_buffer_data(struct ums9117_jpeg_dma_buffer *buffer)
{
	return (u8 *)buffer->allocation + JPEG_GUARD_BYTES;
}

static dma_addr_t jpeg_buffer_dma(struct ums9117_jpeg_dma_buffer *buffer)
{
	return buffer->dma + JPEG_GUARD_BYTES;
}

static void jpeg_free_buffers(struct ums9117_jpeg_hw *hw)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hw->buffers); ++i) {
		struct ums9117_jpeg_dma_buffer *buffer = &hw->buffers[i];

		if (buffer->allocation)
			dma_free_coherent(hw->dev, buffer->allocation_size,
					  buffer->allocation, buffer->dma);
		memset(buffer, 0, sizeof(*buffer));
	}
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
	memset(buffer->allocation, JPEG_GUARD_VALUE, buffer->allocation_size);
	memset(jpeg_buffer_data(buffer), 0, buffer->size);
	return 0;
}

static bool jpeg_guards_ok(struct ums9117_jpeg_hw *hw)
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

static bool jpeg_buffers_disjoint(struct ums9117_jpeg_hw *hw)
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

static u32 jpeg_events(struct ums9117_jpeg_hw *hw)
{
	unsigned long flags;
	u32 events;

	spin_lock_irqsave(&hw->lock, flags);
	events = hw->events;
	spin_unlock_irqrestore(&hw->lock, flags);
	return events;
}

static void jpeg_encode_snapshot_timeout(struct ums9117_jpeg_hw *hw,
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
		.poll_value = poll_valid ? jpeg_read(hw, poll_offset) : 0,
		.bsm_debug = jpeg_read(hw, JPEG_BSM_DEBUG),
		.bsm_write_ready = jpeg_read(hw, JPEG_BSM_WRITE_READY),
		.bsm_bits = jpeg_read(hw, JPEG_BSM_BITS),
		.vlc_control = jpeg_read(hw, JPEG_VLC_CONTROL),
		.ahb = jpeg_read(hw, JPEG_AHB_STATUS),
	};
	unsigned long flags;

	spin_lock_irqsave(&hw->lock, flags);
	snapshot.irq_events = hw->events;
	snapshot.irq_raw = hw->irq_raw;
	hw->stats.encode_timeout = snapshot;
	spin_unlock_irqrestore(&hw->lock, flags);
}

static void jpeg_set_operation(struct ums9117_jpeg_hw *hw,
			       enum ums9117_jpeg_hw_operation operation,
			       u32 irq_mask)
{
	unsigned long flags;

	jpeg_write(hw, JPEG_INT_MASK, 0);
	jpeg_read(hw, JPEG_INT_MASK);
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

static void jpeg_clear_operation(struct ums9117_jpeg_hw *hw)
{
	WRITE_ONCE(hw->active_irq_mask, 0);
	WRITE_ONCE(hw->operation, JPEG_OPERATION_NONE);
}

static void jpeg_reset_submission(struct ums9117_jpeg_hw *hw)
{
	unsigned long flags;

	jpeg_write(hw, JPEG_INT_MASK, 0);
	jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	reinit_completion(&hw->completion);
	spin_lock_irqsave(&hw->lock, flags);
	hw->events = 0;
	hw->irq_raw = 0;
	spin_unlock_irqrestore(&hw->lock, flags);
	jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
}

static unsigned long jpeg_subdeadline(unsigned long deadline)
{
	unsigned long local = jiffies + usecs_to_jiffies(JPEG_POLL_US);

	return time_before(local, deadline) ? local : deadline;
}

static int jpeg_operation_check(struct ums9117_jpeg_hw *hw, u32 error_mask,
				u32 full_mask, unsigned long deadline)
{
	u32 events = jpeg_events(hw) | (jpeg_read(hw, JPEG_INT_RAW) &
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

static int jpeg_operation_poll(struct ums9117_jpeg_hw *hw, u32 reg, u32 mask,
			       u32 expected, u32 error_mask, u32 full_mask,
			       unsigned long deadline)
{
	int ret;

	for (;;) {
		ret = jpeg_operation_check(hw, error_mask, full_mask, deadline);
		if (ret) {
			if (ret == -ETIMEDOUT &&
			    READ_ONCE(hw->operation) == JPEG_OPERATION_ENCODE)
				jpeg_encode_snapshot_timeout(hw, true, reg,
							     mask, expected);
			return ret;
		}
		if ((jpeg_read(hw, reg) & mask) == expected)
			return 0;
		usleep_range(10, 20);
	}
}

static void jpeg_operation_request(struct ums9117_jpeg_hw *hw,
				   enum ums9117_jpeg_hw_operation operation)
{
	unsigned long flags;

	spin_lock_irqsave(&hw->lock, flags);
	hw->operation_events = 0;
	hw->stats.operation[operation].requests++;
	spin_unlock_irqrestore(&hw->lock, flags);
}

static void jpeg_operation_result(struct ums9117_jpeg_hw *hw,
				  enum ums9117_jpeg_hw_operation operation,
				  int result, u64 units, u64 bytes,
				  u64 started_ns)
{
	struct ums9117_jpeg_hw_operation_stats *stats;
	unsigned long flags;
	u32 ahb = jpeg_read(hw, JPEG_AHB_STATUS);

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

static int jpeg_tables(struct ums9117_jpeg_hw *hw,
		       const struct ums9117_jpeg_frame *frame)
{
	u32 value;
	unsigned int i;
	int ret;

	jpeg_write(hw, JPEG_CFG, JPEG_CFG_VSP | JPEG_CFG_CPU_TABLES);
	ret = readl_poll_timeout(hw->base + JPEG_CFG, value,
				 value & JPEG_CFG_CPU_ACK, 10, JPEG_POLL_US);
	if (ret)
		return ret;
	for (i = 0; i < UMS9117_JPEG_QUANT_WORDS; ++i)
		jpeg_write(hw, JPEG_QUANT_TABLE + 4 * i, frame->quant_words[i]);
	for (i = 0; i < UMS9117_JPEG_HUFF_VALUE_WORDS; ++i)
		jpeg_write(hw, JPEG_HUFF_VALUE_TABLE + 4 * i,
			   frame->huff_value_words[i]);
	for (i = 0; i < UMS9117_JPEG_HUFF_META_WORDS; ++i)
		jpeg_write(hw, JPEG_HUFF_META_TABLE + 4 * i,
			   frame->huff_meta_words[i]);
	jpeg_write(hw, JPEG_CFG, JPEG_CFG_VSP);
	return readl_poll_timeout(hw->base + JPEG_CFG, value,
				  !(value & JPEG_CFG_CPU_ACK), 10,
				  JPEG_POLL_US);
}

static void jpeg_reset(struct ums9117_jpeg_hw *hw)
{
	writel(JPEG_RESET, hw->reset_set);
	writel(JPEG_RESET, hw->reset_clear);
}

static int jpeg_stop(struct ums9117_jpeg_hw *hw)
{
	enum ums9117_jpeg_hw_operation operation = READ_ONCE(hw->operation);
	unsigned long flags;
	u32 status;
	int ret;

	jpeg_write(hw, JPEG_INT_MASK, 0);
	readl(hw->base + JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	if (operation == JPEG_OPERATION_DECODE)
		jpeg_snapshot(hw);
	jpeg_write(hw, JPEG_BSM_CFG0,
		   jpeg_read(hw, JPEG_BSM_CFG0) & ~JPEG_BSM_BUFFERS);
	jpeg_write(hw, JPEG_MBIO_ARM, 0);
	jpeg_reset(hw);
	ret = jpeg_wait_idle(hw, &status);
	if (!ret)
		jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);

	spin_lock_irqsave(&hw->lock, flags);
	if (operation > JPEG_OPERATION_NONE && operation < JPEG_OPERATION_COUNT)
		hw->stats.operation[operation].resets++;
	if (operation == JPEG_OPERATION_DECODE) {
		hw->stats.resets++;
		hw->stats.recovery_ahb = status;
	}
	if (ret) {
		if (operation > JPEG_OPERATION_NONE &&
		    operation < JPEG_OPERATION_COUNT)
			hw->stats.operation[operation].idle_failures++;
		if (operation == JPEG_OPERATION_DECODE)
			hw->stats.idle_failures++;
		WRITE_ONCE(hw->failed, true);
	}
	spin_unlock_irqrestore(&hw->lock, flags);
	jpeg_clear_operation(hw);
	return ret;
}

static int jpeg_configure(struct ums9117_jpeg_hw *hw,
			  const struct ums9117_jpeg_frame *frame, size_t y_size,
			  unsigned int factor)
{
	u32 count = frame->mcu_x * frame->mcu_y;
	u32 status;
	int ret;

	jpeg_write(hw, JPEG_INT_MASK, 0);
	jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	jpeg_reset(hw);
	jpeg_write(hw, JPEG_CFG, JPEG_CFG_VSP | JPEG_CFG_CPU_TABLES);
	/* Only one slot is valid; its staging memory is reused after idle/copy. */
	jpeg_write(hw, JPEG_FRAME0_Y,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[1])));
	jpeg_write(hw, JPEG_FRAME0_UV,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[2])));
	jpeg_write(hw, JPEG_FRAME1_Y,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[1])));
	jpeg_write(hw, JPEG_FRAME1_UV,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[2])));
	jpeg_write(hw, JPEG_STREAM0,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[0])));
	jpeg_write(hw, JPEG_STREAM1,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[0])));
	jpeg_write(hw, JPEG_GLOBAL_CFG0, 0x102cb);
	jpeg_write(hw, JPEG_TIMER, 0xffff);
	jpeg_write(hw, JPEG_BURST_GAP, 0x1ff);
	jpeg_write(hw, JPEG_ENDIAN, (y_size >> 2) << 4);
	jpeg_write(hw, JPEG_SOURCE_SIZE,
		   ((frame->padded_height / factor) << 16) |
			   (frame->padded_width / factor));
	jpeg_write(hw, JPEG_BSM_CFG1, BIT(31));
	jpeg_write(hw, JPEG_BSM_CFG0, hw->buffers[0].size / 4);
	jpeg_write(hw, JPEG_VLD_MCU_COUNT, count);
	jpeg_write(hw, JPEG_DCT_CONFIG, (frame->mcu_format << 9) | 0x102);
	jpeg_write(hw, JPEG_DCT_FINISH, 1);
	jpeg_write(hw, JPEG_MBIO_CONFIG, factor == 4 ? 0x2102 : 2);
	ret = jpeg_tables(hw, frame);
	if (ret || atomic_read(&hw->cancelled))
		return ret ?: -ECANCELED;
	jpeg_write(hw, JPEG_GLOBAL_CFG1,
		   (frame->mcu_format << 24) | (frame->mcu_y << 12) |
			   frame->mcu_x);
	dma_wmb();
	jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	jpeg_write(hw, JPEG_INT_MASK, JPEG_DECODE_IRQ_ENABLED);
	jpeg_write(hw, JPEG_BSM_CFG0,
		   JPEG_BSM_BUFFER0 | (hw->buffers[0].size / 4));
	ret = readl_poll_timeout(hw->base + JPEG_BSM_DEBUG, status,
				 status & JPEG_BSM_READY, 10, JPEG_POLL_US);
	if (ret || atomic_read(&hw->cancelled))
		return ret ?: -ECANCELED;
	jpeg_write(hw, JPEG_RESTART_COUNT, 0);
	jpeg_write(hw, JPEG_RESTART_INTERVAL, frame->restart_interval);
	jpeg_write(hw, JPEG_DC_Y, 0);
	jpeg_write(hw, JPEG_DC_UV, 0);
	return 0;
}

static int jpeg_start_slice(struct ums9117_jpeg_hw *hw, u32 count,
			    unsigned int index)
{
	unsigned long flags;
	u32 events;

	jpeg_write(hw, JPEG_INT_MASK, 0);
	jpeg_read(hw, JPEG_INT_MASK);
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
	jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_DONE);
	jpeg_write(hw, JPEG_INT_MASK, JPEG_DECODE_IRQ_ENABLED);
	dma_wmb();
	jpeg_write(hw, JPEG_MBIO_STATUS, BIT(index & 1));
	jpeg_write(hw, JPEG_MBIO_MCU_COUNT, count);
	jpeg_write(hw, JPEG_MBIO_ARM, 1);
	jpeg_write(hw, JPEG_VLD_MCU_COUNT, count);
	jpeg_write(hw, JPEG_VLD_CONTROL, 1);
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
	jpeg_write(hw, JPEG_INT_MASK, 0);
	jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	spin_lock_irqsave(&hw->lock, flags);
	events = hw->events;
	spin_unlock_irqrestore(&hw->lock, flags);
	events |= jpeg_read(hw, JPEG_INT_RAW);
	if (events & JPEG_DECODE_IRQ_ERRORS)
		return -EILSEQ;
	if (!(events & JPEG_IRQ_DONE))
		return -EIO;
	if (time_after_eq(jiffies, deadline))
		return -ETIMEDOUT;
	if (completed_rows == frame->padded_height)
		return (jpeg_read(hw, JPEG_MBIO_STATUS) &
			JPEG_MBIO_FRAME_DONE) ?
			       0 :
			       -EIO;
	progress = jpeg_read(hw, JPEG_MBIO_PROGRESS);
	if ((progress & 0x3ff) + ((progress >> 16) & 0x3ff) * frame->mcu_x <
	    completed_rows / (8 * frame->vertical_subsampling) * frame->mcu_x)
		return -EIO;
	return 0;
}

void ums9117_jpeg_hw_prepare(struct ums9117_jpeg_hw *hw)
{
	unsigned long flags;

	atomic_set(&hw->cancelled, 0);
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
}

bool ums9117_jpeg_hw_failed(struct ums9117_jpeg_hw *hw)
{
	return READ_ONCE(hw->failed);
}

int ums9117_jpeg_hw_decode(struct ums9117_jpeg_hw *hw,
			   const struct ums9117_jpeg_frame *frame,
			   const u8 *entropy, u8 *output[2],
			   const size_t capacity[2], unsigned int factor)
{
	unsigned long flags, deadline;
	u64 started_ns;
	size_t full_y_size, y_size, uv_size, slice_y_size, slice_uv_size;
	size_t y_offset;
	u32 events, row, rows, slice_rows;
	unsigned int completed_slices = 0;
	unsigned int index = 0;
	int ret, stop_ret;

	if (!frame || !entropy || !output || !output[0] || !output[1] ||
	    !capacity || (factor != 1 && factor != 4))
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
	y_size = factor == 4 ? full_y_size / 16 : full_y_size;
	uv_size = y_size / frame->vertical_subsampling;
	if (capacity[0] < y_size || capacity[1] < uv_size)
		return -ENOSPC;
	slice_rows = min_t(u32, frame->padded_height, JPEG_SLICE_ROWS);
	slice_y_size =
		(size_t)(frame->padded_width / factor) * (slice_rows / factor);
	slice_uv_size = slice_y_size / frame->vertical_subsampling;

	started_ns = ktime_get_ns();
	jpeg_operation_request(hw, JPEG_OPERATION_DECODE);
	spin_lock_irqsave(&hw->lock, flags);
	hw->stats.requests++;
	spin_unlock_irqrestore(&hw->lock, flags);
	ret = jpeg_allocate_buffer(hw, &hw->buffers[0],
				   frame->entropy_length +
					   JPEG_STREAM_TAIL_BYTES);
	if (ret)
		goto free_buffers;
	ret = jpeg_allocate_buffer(hw, &hw->buffers[1], slice_y_size);
	if (ret)
		goto free_buffers;
	ret = jpeg_allocate_buffer(hw, &hw->buffers[2], slice_uv_size);
	if (ret)
		goto free_buffers;
	memcpy(jpeg_buffer_data(&hw->buffers[0]), entropy,
	       frame->entropy_length);
	jpeg_set_operation(hw, JPEG_OPERATION_DECODE, JPEG_DECODE_IRQ_ENABLED);
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
		if (!jpeg_guards_ok(hw)) {
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
		memcpy(output[0] + y_offset, jpeg_buffer_data(&hw->buffers[1]),
		       slice_y_size);
		memcpy(output[1] + y_offset / frame->vertical_subsampling,
		       jpeg_buffer_data(&hw->buffers[2]), slice_uv_size);
		memset(jpeg_buffer_data(&hw->buffers[1]), 0,
		       hw->buffers[1].size);
		memset(jpeg_buffer_data(&hw->buffers[2]), 0,
		       hw->buffers[2].size);
		spin_lock_irqsave(&hw->lock, flags);
		hw->stats.slices++;
		spin_unlock_irqrestore(&hw->lock, flags);
		completed_slices++;
	}
stop:
	stop_ret = jpeg_stop(hw);
	if (stop_ret) {
		ret = stop_ret;
		goto record_result;
	}
	spin_lock_irqsave(&hw->lock, flags);
	events = hw->stats.last_raw | hw->stats.last_status;
	spin_unlock_irqrestore(&hw->lock, flags);
	if (!ret && (events & JPEG_DECODE_IRQ_ERRORS))
		ret = -EILSEQ;
	if (!jpeg_guards_ok(hw)) {
		spin_lock_irqsave(&hw->lock, flags);
		hw->stats.guard_failures++;
		hw->stats.operation[JPEG_OPERATION_DECODE].guard_failures++;
		WRITE_ONCE(hw->failed, true);
		spin_unlock_irqrestore(&hw->lock, flags);
		ret = -EIO;
	}
	if (memcmp(jpeg_buffer_data(&hw->buffers[0]), entropy,
		   frame->entropy_length)) {
		spin_lock_irqsave(&hw->lock, flags);
		hw->stats.input_failures++;
		hw->stats.operation[JPEG_OPERATION_DECODE].input_failures++;
		WRITE_ONCE(hw->failed, true);
		spin_unlock_irqrestore(&hw->lock, flags);
		ret = -EIO;
	}
	if (!ret && atomic_read(&hw->cancelled))
		ret = -ECANCELED;
free_buffers:
	jpeg_free_buffers(hw);
record_result:
	jpeg_clear_operation(hw);
	spin_lock_irqsave(&hw->lock, flags);
	hw->stats.last_result = ret;
	if (!ret)
		hw->stats.completed++;
	if (ret == -ETIMEDOUT)
		hw->stats.timeouts++;
	if (ret == -ECANCELED)
		hw->stats.cancellations++;
	spin_unlock_irqrestore(&hw->lock, flags);
	jpeg_operation_result(hw, JPEG_OPERATION_DECODE, ret, completed_slices,
			      y_size + uv_size, started_ns);
	return ret;
}

static int jpeg_encode_owner(struct ums9117_jpeg_hw *hw, bool cpu,
			     unsigned long deadline)
{
	jpeg_write(hw, JPEG_CFG, cpu ? 0x19 : 0x09);
	return jpeg_operation_poll(hw, JPEG_CFG, JPEG_CFG_CPU_ACK,
				   cpu ? JPEG_CFG_CPU_ACK : 0,
				   JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
				   jpeg_subdeadline(deadline));
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

	jpeg_reset_submission(hw);
	jpeg_reset(hw);
	jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	ret = jpeg_encode_owner(hw, true, deadline);
	if (ret)
		return ret;
	jpeg_write(hw, JPEG_SOURCE_SIZE,
		   (config->height << 16) | config->width);
	jpeg_write(hw, JPEG_FRAME0_Y,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[0])));
	jpeg_write(hw, JPEG_FRAME0_UV,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[0])) +
			   strip_plane_size);
	jpeg_write(hw, JPEG_FRAME1_Y,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[1])));
	jpeg_write(hw, JPEG_FRAME1_UV,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[1])) +
			   strip_plane_size);
	jpeg_write(hw, JPEG_STREAM0,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[2])));
	jpeg_write(hw, JPEG_STREAM1,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[3])));
	jpeg_write(hw, JPEG_ENDIAN, 4);
	jpeg_write(hw, JPEG_GLOBAL_CFG0, 0x112b3);
	jpeg_write(hw, JPEG_GLOBAL_CFG1,
		   (UMS9117_JPEG_MCU_422 << 24) | (config->mcu_y << 12) |
			   config->mcu_x);
	jpeg_write(hw, JPEG_TIMER, 0xffff);
	jpeg_write(hw, JPEG_BURST_GAP, 0x1ff);
	jpeg_write(hw, JPEG_ENDIAN,
		   (((config->width * config->height) / 4) << 4) | 4);
	jpeg_write(hw, JPEG_BSM_CFG0, stream_size / 4);
	jpeg_write(hw, JPEG_VLC_MCU_COUNT, count);
	jpeg_write(hw, JPEG_DCT_CONFIG, 0x103);
	jpeg_write(hw, JPEG_DCT_FINISH, 1);
	jpeg_write(hw, JPEG_MBIO_CONFIG, 2);
	for (i = 0; i < UMS9117_JPEG_ENCODE_AC_LUT_WORDS; ++i)
		jpeg_write(hw, JPEG_HUFF_VALUE_TABLE + i * 4,
			   config->ac_lut_words[i]);
	ret = jpeg_encode_owner(hw, false, deadline);
	if (ret)
		return ret;
	ret = jpeg_encode_owner(hw, true, deadline);
	if (ret)
		return ret;
	for (i = 0; i < UMS9117_JPEG_ENCODE_QBUF_WORDS; ++i)
		jpeg_write(hw, JPEG_QUANT_TABLE + i * 4, config->qbuf_words[i]);
	ret = jpeg_encode_owner(hw, false, deadline);
	if (!ret) {
		jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
		jpeg_write(hw, JPEG_INT_MASK, JPEG_ENCODE_IRQ_ENABLED);
		jpeg_read(hw, JPEG_INT_MASK);
	}
	return ret;
}

static int jpeg_encode_bsm_write(struct ums9117_jpeg_hw *hw, u32 bits,
				 u32 value, unsigned long deadline)
{
	int ret;

	ret = jpeg_operation_poll(hw, JPEG_BSM_WRITE_READY, BIT(0), BIT(0),
				  JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
				  deadline);
	if (ret)
		return ret;
	jpeg_write(hw, JPEG_BSM_CONTROL, bits << 24);
	jpeg_write(hw, JPEG_BSM_DATA, value);
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
		ret = jpeg_operation_check(hw, JPEG_ENCODE_IRQ_ERRORS,
					   JPEG_IRQ_BSM, deadline);
		if (ret) {
			if (ret == -ETIMEDOUT)
				jpeg_encode_snapshot_timeout(hw, false, 0, 0,
							     0);
			return ret;
		}
		events = jpeg_events(hw);
		if ((events & done) == done)
			return 0;
		now = jiffies;
		if (time_after_eq(now, deadline) ||
		    !wait_for_completion_timeout(&hw->completion,
						 deadline - now)) {
			jpeg_encode_snapshot_timeout(hw, false, 0, 0, 0);
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
		u8 *data = jpeg_buffer_data(&hw->buffers[bank]);

		memcpy(data, input[0] + offset, strip_plane_size);
		memcpy(data + strip_plane_size, input[1] + offset,
		       strip_plane_size);
	}
	memset(jpeg_buffer_data(&hw->buffers[2]), JPEG_ENCODE_FILL,
	       hw->buffers[2].size);
	memset(jpeg_buffer_data(&hw->buffers[3]), JPEG_ENCODE_FILL,
	       hw->buffers[3].size);
}

static int jpeg_encode_verify_memory(struct ums9117_jpeg_hw *hw,
				     const u8 *input[2], size_t offset,
				     size_t strip_plane_size,
				     size_t stream_size)
{
	unsigned int bank;

	if (!jpeg_guards_ok(hw))
		return -EFAULT;
	for (bank = 0; bank < 2; ++bank) {
		u8 *data = jpeg_buffer_data(&hw->buffers[bank]);

		if (memcmp(data, input[0] + offset, strip_plane_size) ||
		    memcmp(data + strip_plane_size, input[1] + offset,
			   strip_plane_size))
			return -EBADMSG;
	}
	for (bank = 2; bank < 4; ++bank) {
		u8 *data = jpeg_buffer_data(&hw->buffers[bank]);

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
	jpeg_write(hw, JPEG_ENDIAN, ((strip_plane_size / 4) << 4) | 4);
	jpeg_write(hw, JPEG_VLC_MCU_COUNT, count);
	jpeg_write(hw, JPEG_MBIO_MCU_COUNT, count);
	jpeg_write(hw, JPEG_MBIO_STATUS, 1);
	jpeg_write(hw, JPEG_MBIO_ARM, 1);
	ret = jpeg_encode_wait_done(hw, deadline);
	if (ret)
		return ret;
	/* MBIO completion releases the input, not pending AHB transfers. */
	ret = jpeg_operation_poll(hw, JPEG_AHB_STATUS, JPEG_AHB_BUSY, 0,
				  JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
				  deadline);
	if (ret)
		return ret;
	if (strip + 1 == strips) {
		ret = jpeg_operation_poll(hw, JPEG_VLC_CONTROL, BIT(31), 0,
					  JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
					  deadline);
		if (ret)
			return ret;
	}
	jpeg_write(hw, JPEG_VLC_CONTROL, 1);
	if (strip + 1 < strips) {
		ret = jpeg_encode_bsm_write(hw, 8, 0xff, deadline);
		if (!ret)
			ret = jpeg_encode_bsm_write(hw, 8, 0xd0 + (strip & 7),
						    deadline);
	} else {
		ret = jpeg_encode_bsm_write(hw, 16, 0xffd9, deadline);
		if (!ret)
			jpeg_write(hw, JPEG_VLC_CONTROL, 1);
	}
	if (ret)
		return ret;
	ret = jpeg_operation_poll(hw, JPEG_BSM_WRITE_READY, BIT(0), BIT(0),
				  JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
				  deadline);
	if (ret)
		return ret;
	jpeg_write(hw, JPEG_BSM_CONTROL, 2);
	ret = jpeg_operation_poll(hw, JPEG_BSM_DEBUG, JPEG_BSM_READY,
				  JPEG_BSM_READY, JPEG_ENCODE_IRQ_ERRORS,
				  JPEG_IRQ_BSM, deadline);
	if (ret)
		return ret;
	bits = jpeg_read(hw, JPEG_BSM_BITS);
	if (!bits || (bits & 7) || bits / 8 >= stream_size)
		return -EOVERFLOW;
	ret = jpeg_operation_poll(hw, JPEG_AHB_STATUS, JPEG_AHB_BUSY, 0,
				  JPEG_ENCODE_IRQ_ERRORS, JPEG_IRQ_BSM,
				  deadline);
	if (ret)
		return ret;
	ret = jpeg_encode_verify_memory(hw, input, input_offset,
					strip_plane_size, stream_size);
	if (ret)
		return ret;
	if (!memchr_inv(jpeg_buffer_data(&hw->buffers[2]), JPEG_ENCODE_FILL,
			bits / 8))
		return -ENODATA;
	if (bits / 8 > capacity - *result_size)
		return -ENOSPC;
	dma_rmb();
	memcpy(output + *result_size, jpeg_buffer_data(&hw->buffers[2]),
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
	jpeg_operation_request(hw, JPEG_OPERATION_ENCODE);
	ret = jpeg_allocate_buffer(hw, &hw->buffers[0], 2 * strip_plane_size);
	if (ret)
		goto free_buffers;
	ret = jpeg_allocate_buffer(hw, &hw->buffers[1], 2 * strip_plane_size);
	if (ret)
		goto free_buffers;
	ret = jpeg_allocate_buffer(hw, &hw->buffers[2], stream_capacity);
	if (ret)
		goto free_buffers;
	ret = jpeg_allocate_buffer(hw, &hw->buffers[3], stream_capacity);
	if (ret)
		goto free_buffers;
	if (!jpeg_buffers_disjoint(hw)) {
		ret = -ERANGE;
		goto free_buffers;
	}
	jpeg_set_operation(hw, JPEG_OPERATION_ENCODE, JPEG_ENCODE_IRQ_ENABLED);
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
	stop_ret = jpeg_stop(hw);
	if (stop_ret) {
		ret = stop_ret;
		goto record_result;
	}
	memory_ret = jpeg_encode_verify_memory(
		hw, input, (size_t)staged_strip * strip_plane_size,
		strip_plane_size, staged_stream_size);
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
	jpeg_free_buffers(hw);
record_result:
	jpeg_clear_operation(hw);
	jpeg_operation_result(hw, JPEG_OPERATION_ENCODE, ret, completed_strips,
			      result_size, started_ns);
	return ret;
}

static void jpeg_scale_t4_copy(u8 *destination, const u8 *source, size_t size)
{
	size_t i;

	for (i = 0; i < size; ++i)
		destination[i] = source[i ^ 3];
}

static void
jpeg_scale_fill_inputs(struct ums9117_jpeg_hw *hw,
		       const struct ums9117_jpeg_scale_config *config,
		       const u8 *input[2])
{
	unsigned int plane;

	for (plane = 0; plane < 2; ++plane)
		jpeg_scale_t4_copy(jpeg_buffer_data(&hw->buffers[plane]),
				   input[plane], config->source_bytes);
	for (plane = 2; plane < 4; ++plane)
		memset(jpeg_buffer_data(&hw->buffers[plane]), JPEG_ENCODE_FILL,
		       config->dest_bytes);
}

static int
jpeg_scale_verify_memory(struct ums9117_jpeg_hw *hw,
			 const struct ums9117_jpeg_scale_config *config,
			 const u8 *input[2])
{
	unsigned int plane;
	size_t i;

	if (!jpeg_guards_ok(hw))
		return -EFAULT;
	dma_rmb();
	for (plane = 0; plane < 2; ++plane) {
		const u8 *data = jpeg_buffer_data(&hw->buffers[plane]);

		for (i = 0; i < config->source_bytes; ++i)
			if (data[i] != input[plane][i ^ 3])
				return -EBADMSG;
	}
	return 0;
}

static int jpeg_scale_owner(struct ums9117_jpeg_hw *hw, bool cpu,
			    unsigned long deadline)
{
	jpeg_write(hw, JPEG_CFG, cpu ? 0x12 : 0x02);
	return jpeg_operation_poll(hw, JPEG_CFG, JPEG_CFG_CPU_ACK,
				   cpu ? JPEG_CFG_CPU_ACK : 0,
				   JPEG_SCALE_IRQ_ERRORS, 0,
				   jpeg_subdeadline(deadline));
}

static int jpeg_scale_upload_tables(struct ums9117_jpeg_hw *hw)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(jpeg_scale_h); ++i) {
		jpeg_write(hw, JPEG_SCALER_H_TABLE + i * 4, jpeg_scale_h[i]);
		if (jpeg_read(hw, JPEG_SCALER_H_TABLE + i * 4) !=
		    jpeg_scale_h[i])
			return -EIO;
	}
	for (i = 0; i < ARRAY_SIZE(jpeg_scale_v); ++i) {
		jpeg_write(hw, JPEG_SCALER_V_TABLE + i * 4, jpeg_scale_v[i]);
		if (jpeg_read(hw, JPEG_SCALER_V_TABLE + i * 4) !=
		    jpeg_scale_v[i])
			return -EIO;
	}
	return 0;
}

static int jpeg_scale_configure(struct ums9117_jpeg_hw *hw,
				const struct ums9117_jpeg_scale_config *config,
				unsigned long deadline)
{
	int ret;

	jpeg_reset_submission(hw);
	ret = jpeg_scale_owner(hw, true, deadline);
	if (ret)
		return ret;
	ret = jpeg_scale_upload_tables(hw);
	if (ret)
		return ret;
	ret = jpeg_scale_owner(hw, false, deadline);
	if (ret)
		return ret;
	jpeg_write(hw, JPEG_SC_FORMAT, 0);
	jpeg_write(hw, JPEG_SOURCE_SIZE,
		   (config->source_height << 16) | config->source_width);
	jpeg_write(hw, JPEG_DEST_SIZE,
		   (config->dest_height << 16) | config->dest_width);
	jpeg_write(hw, JPEG_FRAME0_Y,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[0])));
	jpeg_write(hw, JPEG_FRAME0_UV,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[1])));
	jpeg_write(hw, JPEG_FRAME1_Y, 0);
	jpeg_write(hw, JPEG_FRAME1_UV, 0);
	jpeg_write(hw, JPEG_STREAM0,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[2])));
	jpeg_write(hw, JPEG_STREAM1,
		   lower_32_bits(jpeg_buffer_dma(&hw->buffers[3])));
	jpeg_write(hw, JPEG_FRAME6, 0);
	jpeg_write(hw, JPEG_BURST_GAP, 0);
	jpeg_write(hw, JPEG_ENDIAN, 5);
	jpeg_write(hw, JPEG_TRIM_START, 0);
	jpeg_write(hw, JPEG_TRIM_SIZE,
		   (config->source_height << 16) | config->source_width);
	jpeg_write(hw, JPEG_SLICE_SIZE, 0);
	jpeg_write(hw, JPEG_SCALING_CONFIG, 0x200);
	return 0;
}

static int jpeg_scale_wait(struct ums9117_jpeg_hw *hw, unsigned long deadline)
{
	unsigned long now;
	int ret;

	dma_wmb();
	jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	jpeg_write(hw, JPEG_INT_MASK, JPEG_SCALE_IRQ_ENABLED);
	jpeg_read(hw, JPEG_INT_MASK);
	jpeg_write(hw, JPEG_SCALING_CONFIG, 0x201);
	now = jiffies;
	if (time_after_eq(now, deadline) ||
	    !wait_for_completion_timeout(&hw->completion, deadline - now))
		return -ETIMEDOUT;
	ret = jpeg_operation_check(hw, JPEG_SCALE_IRQ_ERRORS, 0, deadline);
	return ret;
}

static int jpeg_scale_finish(struct ums9117_jpeg_hw *hw, unsigned long deadline)
{
	struct ums9117_jpeg_hw_operation_stats *stats;
	unsigned long flags;
	u32 events, irq_count;
	int ret;

	jpeg_write(hw, JPEG_INT_MASK, 0);
	jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	spin_lock_irqsave(&hw->lock, flags);
	events = hw->events | (hw->irq_raw & JPEG_SCALE_IRQ_ENABLED);
	irq_count = hw->active_irq_count;
	spin_unlock_irqrestore(&hw->lock, flags);
	if (!irq_count || !(jpeg_events(hw) & JPEG_SCALE_IRQ_DONE) ||
	    (events & JPEG_SCALE_IRQ_ENABLED) != JPEG_SCALE_IRQ_DONE)
		return -EIO;
	ret = jpeg_operation_poll(hw, JPEG_SCALING_CONFIG, BIT(0), 0,
				  JPEG_SCALE_IRQ_ERRORS, 0,
				  jpeg_subdeadline(deadline));
	if (ret)
		return ret;
	ret = jpeg_operation_poll(hw, JPEG_AHB_STATUS, JPEG_AHB_BUSY, 0,
				  JPEG_SCALE_IRQ_ERRORS, 0,
				  jpeg_subdeadline(deadline));
	if (ret)
		return ret;
	spin_lock_irqsave(&hw->lock, flags);
	stats = &hw->stats.operation[JPEG_OPERATION_SCALE];
	stats->last_command = jpeg_read(hw, JPEG_SCALING_CONFIG);
	spin_unlock_irqrestore(&hw->lock, flags);
	jpeg_write(hw, JPEG_CFG, 0);
	jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	jpeg_clear_operation(hw);
	return 0;
}

static int jpeg_scale_recover(struct ums9117_jpeg_hw *hw)
{
	struct ums9117_jpeg_hw_operation_stats *stats;
	unsigned long flags;
	u32 status;
	int ret;

	jpeg_write(hw, JPEG_INT_MASK, 0);
	jpeg_read(hw, JPEG_INT_MASK);
	synchronize_irq(hw->irq);
	writel(JPEG_DCAM_RESET, hw->reset_set);
	writel(JPEG_DCAM_RESET, hw->reset_clear);
	ret = readl_poll_timeout(hw->base + JPEG_AHB_STATUS, status,
				 !(status & JPEG_AHB_BUSY), 10, JPEG_POLL_US);
	spin_lock_irqsave(&hw->lock, flags);
	stats = &hw->stats.operation[JPEG_OPERATION_SCALE];
	stats->resets++;
	stats->recovery_ahb = status;
	stats->last_command = jpeg_read(hw, JPEG_SCALING_CONFIG);
	if (ret) {
		stats->idle_failures++;
		WRITE_ONCE(hw->failed, true);
	}
	spin_unlock_irqrestore(&hw->lock, flags);
	jpeg_clear_operation(hw);
	if (!ret) {
		jpeg_write(hw, JPEG_CFG, 0);
		jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	}
	return ret;
}

int ums9117_jpeg_hw_scale(struct ums9117_jpeg_hw *hw,
			  enum ums9117_jpeg_scale_profile profile,
			  const u8 *input[2], const size_t input_size[2],
			  u8 *output[2], const size_t capacity[2])
{
	const struct ums9117_jpeg_scale_config *config;
	unsigned long flags, deadline;
	u64 started_ns;
	unsigned int completed_units = 0;
	int memory_ret, recovery_ret, ret;

	if (!input || !input[0] || !input[1] || !input_size || !output ||
	    !output[0] || !output[1] || !capacity)
		return -EINVAL;
	if ((unsigned int)profile >= UMS9117_JPEG_SCALE_PROFILE_COUNT)
		return -EINVAL;
	config = &jpeg_scale_configs[profile];
	if (ums9117_jpeg_hw_failed(hw))
		return -EIO;
	if (atomic_read(&hw->cancelled))
		return -ECANCELED;
	if (input_size[0] != config->source_bytes ||
	    input_size[1] != config->source_bytes)
		return -EINVAL;
	if (capacity[0] < config->dest_bytes ||
	    capacity[1] < config->dest_bytes)
		return -ENOSPC;
	started_ns = ktime_get_ns();
	jpeg_operation_request(hw, JPEG_OPERATION_SCALE);
	ret = jpeg_allocate_buffer(hw, &hw->buffers[0], config->source_bytes);
	if (ret)
		goto free_buffers;
	ret = jpeg_allocate_buffer(hw, &hw->buffers[1], config->source_bytes);
	if (ret)
		goto free_buffers;
	ret = jpeg_allocate_buffer(hw, &hw->buffers[2], config->dest_bytes);
	if (ret)
		goto free_buffers;
	ret = jpeg_allocate_buffer(hw, &hw->buffers[3], config->dest_bytes);
	if (ret)
		goto free_buffers;
	if (!jpeg_buffers_disjoint(hw)) {
		ret = -ERANGE;
		goto free_buffers;
	}
	jpeg_scale_fill_inputs(hw, config, input);
	jpeg_set_operation(hw, JPEG_OPERATION_SCALE, JPEG_SCALE_IRQ_ENABLED);
	deadline = jiffies + msecs_to_jiffies(JPEG_WAIT_MS);
	ret = jpeg_scale_configure(hw, config, deadline);
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
	memory_ret = jpeg_scale_verify_memory(hw, config, input);
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
		jpeg_scale_t4_copy(output[0], jpeg_buffer_data(&hw->buffers[2]),
				   config->dest_bytes);
		jpeg_scale_t4_copy(output[1], jpeg_buffer_data(&hw->buffers[3]),
				   config->dest_bytes);
	}
free_buffers:
	jpeg_free_buffers(hw);
record_result:
	jpeg_clear_operation(hw);
	jpeg_operation_result(hw, JPEG_OPERATION_SCALE, ret, completed_units,
			      ret ? 0 : 2 * config->dest_bytes, started_ns);
	return ret;
}

static int jpeg_stats_show(struct seq_file *seq, void *unused)
{
	static const char *const operation_names[JPEG_OPERATION_COUNT] = {
		[JPEG_OPERATION_DECODE] = "decode",
		[JPEG_OPERATION_ENCODE] = "encode",
		[JPEG_OPERATION_SCALE] = "scale",
	};
	struct ums9117_jpeg_hw *hw = seq->private;
	struct ums9117_jpeg_hw_stats stats;
	unsigned long flags;
	unsigned int operation;

	spin_lock_irqsave(&hw->lock, flags);
	stats = hw->stats;
	spin_unlock_irqrestore(&hw->lock, flags);
	seq_printf(seq,
		   "failed=%u requests=%llu completed=%llu last_result=%d\n",
		   ums9117_jpeg_hw_failed(hw), stats.requests, stats.completed,
		   stats.last_result);
	seq_printf(seq,
		   "irqs=%llu done_irqs=%llu bsm_irqs=%llu error_irqs=%llu\n",
		   stats.irqs, stats.done_irqs, stats.bsm_irqs,
		   stats.error_irqs);
	seq_printf(seq, "slices=%llu slice_rows=%u\n", stats.slices,
		   JPEG_SLICE_ROWS);
	seq_printf(
		seq,
		"timeouts=%llu cancellations=%llu resets=%llu guard_failures=%llu input_failures=%llu idle_failures=%llu\n",
		stats.timeouts, stats.cancellations, stats.resets,
		stats.guard_failures, stats.input_failures,
		stats.idle_failures);
	seq_printf(
		seq,
		"status=%08x raw=%08x mbio=%08x progress=%08x ahb=%08x recovery_ahb=%08x\n",
		stats.last_status, stats.last_raw, stats.last_mbio,
		stats.last_progress, stats.last_ahb, stats.recovery_ahb);
	seq_printf(seq, "bsm_cfg0=%08x bsm_cfg1=%08x bsm_debug=%08x vld=%08x\n",
		   stats.last_bsm_cfg0, stats.last_bsm_cfg1,
		   stats.last_bsm_debug, stats.last_vld);
	for (operation = JPEG_OPERATION_DECODE;
	     operation < JPEG_OPERATION_COUNT; ++operation) {
		const struct ums9117_jpeg_hw_operation_stats *op =
			&stats.operation[operation];

		seq_printf(
			seq,
			"%s requests=%llu completed=%llu units=%llu last_result=%d bytes=%llu duration_ns=%llu\n",
			operation_names[operation], op->requests, op->completed,
			op->units, op->last_result, op->last_bytes,
			op->last_duration_ns);
		seq_printf(
			seq,
			"%s irqs=%llu completion_irqs=%llu error_irqs=%llu\n",
			operation_names[operation], op->irqs,
			op->completion_irqs, op->error_irqs);
		seq_printf(
			seq,
			"%s events=%08x ahb=%08x command=%08x recovery_ahb=%08x\n",
			operation_names[operation], op->last_events,
			op->last_ahb, op->last_command, op->recovery_ahb);
		seq_printf(seq,
			   "%s timeouts=%llu cancellations=%llu resets=%llu\n",
			   operation_names[operation], op->timeouts,
			   op->cancellations, op->resets);
		seq_printf(
			seq,
			"%s guard_failures=%llu input_failures=%llu output_failures=%llu idle_failures=%llu\n",
			operation_names[operation], op->guard_failures,
			op->input_failures, op->output_failures,
			op->idle_failures);
	}
	seq_printf(
		seq,
		"encode_timeout valid=%d strip=%u poll_valid=%d poll_offset=%08x poll_mask=%08x poll_expected=%08x poll_value=%08x irq_events=%08x irq_raw=%08x\n",
		stats.encode_timeout.valid, stats.encode_timeout.strip,
		stats.encode_timeout.poll_valid,
		stats.encode_timeout.poll_offset,
		stats.encode_timeout.poll_mask,
		stats.encode_timeout.poll_expected,
		stats.encode_timeout.poll_value,
		stats.encode_timeout.irq_events, stats.encode_timeout.irq_raw);
	seq_printf(
		seq,
		"encode_timeout bsm_debug=%08x bsm_write_ready=%08x bsm_bits=%08x vlc_control=%08x ahb=%08x\n",
		stats.encode_timeout.bsm_debug,
		stats.encode_timeout.bsm_write_ready,
		stats.encode_timeout.bsm_bits, stats.encode_timeout.vlc_control,
		stats.encode_timeout.ahb);
	seq_printf(
		seq,
		"saved_gate=%08x saved_clock=%08x configured_clock_mask=00000303 configured_clock_value=00000000\n",
		hw->saved_gate, hw->saved_clock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(jpeg_stats);

static void __iomem *jpeg_map_shared(struct platform_device *pdev,
				     const char *name, resource_size_t address)
{
	struct resource *resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	void __iomem *mapping;

	if (!resource || resource->start != address ||
	    resource_size(resource) != 4)
		return IOMEM_ERR_PTR(-EINVAL);
	mapping = devm_ioremap(&pdev->dev, resource->start,
			       resource_size(resource));
	return mapping ?: IOMEM_ERR_PTR(-ENOMEM);
}

static void jpeg_restore_clock_gate(struct ums9117_jpeg_hw *hw)
{
	u32 value = readl(hw->clock);

	writel((value & ~JPEG_CLOCK_MASK) | hw->saved_clock, hw->clock);
	if (!hw->saved_gate)
		writel(JPEG_GATE, hw->gate_clear);
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
	hw->gate_state = jpeg_map_shared(pdev, "ap-ahb-gate-state", 0x20e00000);
	hw->gate_set = jpeg_map_shared(pdev, "ap-ahb-gate-set", 0x20e01000);
	hw->gate_clear = jpeg_map_shared(pdev, "ap-ahb-gate-clear", 0x20e02000);
	hw->reset_set = jpeg_map_shared(pdev, "ap-ahb-reset-set", 0x20e01004);
	hw->reset_clear =
		jpeg_map_shared(pdev, "ap-ahb-reset-clear", 0x20e02004);
	hw->clock = jpeg_map_shared(pdev, "dcam-clock", 0x21500088);
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
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ERR_PTR(ret);
	hw->irq = platform_get_irq(pdev, 0);
	if (hw->irq < 0)
		return ERR_PTR(hw->irq);
	spin_lock_init(&hw->lock);
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
	jpeg_write(hw, JPEG_INT_MASK, 0);
	jpeg_write(hw, JPEG_INT_CLEAR, JPEG_IRQ_KNOWN);
	ret = devm_request_irq(dev, hw->irq, jpeg_irq, 0, dev_name(dev), hw);
	if (ret) {
		jpeg_restore_clock_gate(hw);
		return ERR_PTR(ret);
	}
	hw->debugfs = debugfs_create_dir("ums9117-jpeg", NULL);
	debugfs_create_file("stats", 0444, hw->debugfs, hw, &jpeg_stats_fops);
	return hw;
}

void ums9117_jpeg_hw_destroy(struct ums9117_jpeg_hw *hw)
{
	debugfs_remove_recursive(hw->debugfs);
	jpeg_write(hw, JPEG_INT_MASK, 0);
	devm_free_irq(hw->dev, hw->irq, hw);
	if (ums9117_jpeg_hw_failed(hw))
		return;
	jpeg_free_buffers(hw);
	jpeg_restore_clock_gate(hw);
}
