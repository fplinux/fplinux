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

static int jpeg_stats_show(struct seq_file *seq, void *unused)
{
	static const char *const operation_names[JPEG_OPERATION_COUNT] = {
		[JPEG_OPERATION_DECODE] = "decode",
		[JPEG_OPERATION_ENCODE] = "encode",
		[JPEG_OPERATION_SCALE] = "scale",
	};
	struct ums9117_jpeg_hw *hw = seq->private;
	struct ums9117_jpeg_hw_stats stats;
	const struct ums9117_jpeg_hw_operation_stats *decode =
		&stats.operation[JPEG_OPERATION_DECODE];
	unsigned long flags;
	unsigned int operation;

	spin_lock_irqsave(&hw->lock, flags);
	stats = hw->stats;
	spin_unlock_irqrestore(&hw->lock, flags);
	seq_printf(seq,
		   "failed=%u requests=%llu completed=%llu last_result=%d\n",
		   ums9117_jpeg_hw_failed(hw), decode->requests,
		   decode->completed, stats.last_result);
	seq_printf(seq,
		   "irqs=%llu done_irqs=%llu bsm_irqs=%llu error_irqs=%llu\n",
		   decode->irqs, decode->completion_irqs, stats.bsm_irqs,
		   decode->error_irqs);
	seq_printf(seq, "slices=%llu slice_rows=%u\n", stats.slices,
		   JPEG_SLICE_ROWS);
	seq_printf(
		seq,
		"timeouts=%llu cancellations=%llu resets=%llu guard_failures=%llu input_failures=%llu idle_failures=%llu\n",
		decode->timeouts, decode->cancellations, decode->resets,
		decode->guard_failures, decode->input_failures,
		decode->idle_failures);
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
	seq_printf(seq, "codec_clock_selector=%u current_clock=%08x\n",
		   JPEG_CODEC_CLOCK_SELECTOR, readl(hw->clock));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(jpeg_stats);

void ums9117_jpeg_stats_create(struct ums9117_jpeg_hw *hw)
{
	hw->debugfs = debugfs_create_dir("ums9117-jpeg", NULL);
	debugfs_create_file("stats", 0444, hw->debugfs, hw, &jpeg_stats_fops);
}
