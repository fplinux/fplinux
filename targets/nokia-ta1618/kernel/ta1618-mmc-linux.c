// SPDX-License-Identifier: GPL-2.0-only
/*
 * Board-specific Linux MMC host for the UMS9117 SDIO0 instance in Nokia
 * TA-1618, driving the removable microSD slot.
 *
 * This is deliberately not an SDHCI driver.  UMS9117 has a related command,
 * response, interrupt and ADMA layout, but +0x28 is a 32-bit custom host
 * control register and there is no SDHCI POWER_CONTROL byte.  Platform gate,
 * reset, selector, pin and ADI/PMIC sequencing is fixed to the hardware-proven
 * TA-1618 SDIO0 recipe.
 *
 * Identification runs at 399590 Hz over a 1-bit bus.  Only a clean RCA-scoped
 * CMD55 APP_CMD response and clean no-data ACMD6 argument 2 permit the physical
 * transition to a 4-bit bus.  Data then runs at 24.375 MHz, or 48.75 MHz after
 * the card agrees to SD High Speed, with up to 32 ADMA2 segments and 128 KiB per
 * request.  Multi-block reads and writes use automatic CMD12.  UHS, 1.8 V,
 * tuning, CMD23, erase, discard, lock and write-protect commands are not
 * supported.
 *
 * The MMC core polls the active-low EIC2 TF_DET input, so an unmounted card can
 * be inserted, removed and reinserted while Linux remains running.  Filesystems
 * must be synchronized and unmounted before removal.
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mmc/card.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/soc/sprd/ums9117-adi.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "ta1618-sdio-board.h"

#define TA1618_MMC_IOS_TRACE_DEPTH 16U

#define UMS9117_MMC_BASE_CLOCK_HZ 195000000U
#define UMS9117_MMC_IDENT_REQUEST_MAX_HZ 400000U
#define UMS9117_MMC_CMD6_CHECK_ARG 0x00fffff0U
#define UMS9117_MMC_CMD6_SWITCH_ARG 0x80fffff1U

#define UMS9117_MMC_SIGNAL_COMMAND \
	(UMS9117_SDIO_INT_RESPONSE | UMS9117_SDIO_DETAIL_ERROR_MASK)
#define UMS9117_MMC_SIGNAL_DATA \
	(UMS9117_SDIO_INT_TRANSFER | UMS9117_SDIO_DETAIL_ERROR_MASK)

#define UMS9117_MMC_ADMA2_DESC_COUNT 32U
#define UMS9117_MMC_ADMA2_TABLE_BYTES \
	(UMS9117_MMC_ADMA2_DESC_COUNT * UMS9117_SDIO_ADMA2_DESC_BYTES)
#define UMS9117_MMC_MAX_REQUEST_BYTES 131072U

#define UMS9117_MMC_READ_QUIESCE_DEADLINE_MS 100U
#define UMS9117_MMC_WRITE_QUIESCE_DEADLINE_MS 5000U
#define UMS9117_MMC_REQUEST_TIMEOUT_MS 1000U
#define UMS9117_MMC_WRITE_REQUEST_TIMEOUT_MS 5000U
#define UMS9117_MMC_READ_BLOCK_BUDGET_MS 10U
#define UMS9117_MMC_WRITE_BLOCK_BUDGET_MS 50U
#define UMS9117_MMC_SHUTDOWN_DEADLINE_MS             \
	(UMS9117_MMC_WRITE_REQUEST_TIMEOUT_MS +      \
	 (UMS9117_MMC_MAX_REQUEST_BYTES / 512U) *    \
		 UMS9117_MMC_WRITE_BLOCK_BUDGET_MS + \
	 UMS9117_MMC_WRITE_QUIESCE_DEADLINE_MS)
#define UMS9117_MMC_SHUTDOWN_POLL_MS 1U

static_assert(sizeof(struct ums9117_sdio_adma2_desc) ==
	      UMS9117_SDIO_ADMA2_DESC_BYTES);
static_assert(UMS9117_SDIO_TRANSFER_READ_ADMA2 == 0x0013U);
static_assert(UMS9117_SDIO_TRANSFER_WRITE_ADMA2 == 0x0003U);
static_assert(UMS9117_SDIO_TRANSFER_CMD18_AUTO_CMD12_ADMA2 == 0x0037U);
static_assert(UMS9117_SDIO_TRANSFER_CMD25_AUTO_CMD12_ADMA2 == 0x0027U);
static_assert(UMS9117_SDIO_ADMA2_TRANSFER == 0x0021U);
static_assert(UMS9117_SDIO_ADMA2_TRANSFER_END == 0x0023U);
static_assert(UMS9117_MMC_ADMA2_TABLE_BYTES == 256U);
static_assert(UMS9117_SDIO_HOST_CTRL1_4BIT_ADMA2 == 0x00000012U);
static_assert(UMS9117_SDIO_LEGACY_CLOCK_HZ == 24375000U);
static_assert(UMS9117_SDIO_HS_CLOCK_HZ == 48750000U);
static_assert(UMS9117_MMC_BASE_CLOCK_HZ == 195000000U);
static_assert((MMC_VDD_29_30 | MMC_VDD_30_31) == 0x00060000U);
static_assert(TA1618_SDIO0_INTID == 89U);

struct ta1618_mmc_ios_trace_entry {
	u32 sequence;
	u32 clock;
	u32 power_mode;
	u32 bus_width;
	u32 timing;
	int result;
};

struct ta1618_mmc_audit {
	struct ta1618_mmc_ios_trace_entry ios[TA1618_MMC_IOS_TRACE_DEPTH];
	u32 ios_calls;
	u32 ios_trace_count;
	u32 cmd55_attempt_count;
	u32 cmd55_clean_count;
	u32 cmd55_app_cmd_count;
	u32 cmd55_last_arg;
	u32 cmd55_last_response;
	u32 acmd6_attempt_count;
	u32 acmd6_clean_count;
	u32 acmd6_failure_count;
	u32 cmd17_count;
	u32 cmd24_count;
	u32 cmd18_count;
	u32 cmd25_count;
	u32 max_data_blocks;
	u32 max_descriptors;
	u32 cmd6_check_count;
	u32 cmd6_switch_count;
	u32 irq_count;
	u32 irq_error_count;
	u32 irq_crc_count;
	u32 irq_end_bit_count;
	u32 irq_timeout_count;
	u32 irq_command_timeout_count;
	u32 irq_data_timeout_count;
	u32 irq_auto_cmd12_count;
	u32 host_control2_after_reset;
	u32 irq_adma_error_count;
	u32 watchdog_timeout_count;
	u32 last_irq_raw_status;
	u32 last_irq_owned_status;
	u32 last_irq_w1c_readback;
	u32 last_opcode;
	u32 last_argument;
	u32 last_lba;
	u32 last_cmd17_ordinal;
	bool last_command_valid;
	bool last_lba_valid;
	u32 host_ctrl1_width_before;
	u32 host_ctrl1_width_after;
	u32 selector_after_activate;
	u32 clock_reset_after_activate;
	u32 selector_width_before;
	u32 selector_width_after;
	u32 clock_width_before;
	u32 clock_width_after;
	u32 timeout_field_before;
	u32 timeout_field_candidate;
	u32 timeout_field_readback;
	u32 timeout_register_before;
	u32 timeout_register_candidate;
	u32 timeout_register_readback;
	u32 requested_clock_hz;
	u32 deferred_clock_hz;
	u32 applied_clock_hz;
	u32 deferred_clock_count;
	u32 applied_clock_count;
};

struct ta1618_mmc_host {
	struct device *dev;
	struct mmc_host *mmc;
	void __iomem *controller_regs[UMS9117_SDIO_REG_COUNT];
	void __iomem *board_regs[TA1618_SDIO_REG_BOARD_COUNT];
	struct ta1618_sdio_io sdio;
	struct ta1618_sdio_state sdio_state;
	int irq;
	bool irq_requested;

	spinlock_t lock;
	struct mutex state_mutex;
	struct mmc_request *active_mrq;
	struct mmc_request *finish_mrq;
	struct delayed_work timeout_work;
	struct work_struct finish_work;
	u32 finish_status;
	u32 finish_auto_cmd;
	u32 finish_response[4];
	bool finish_ack_failed;
	bool finish_needs_quiesce;
	bool active_width_acmd6;
	bool finish_width_acmd6;

	struct ums9117_sdio_adma2_desc *descriptors;
	dma_addr_t descriptors_dma;
	bool data_mapped;
	enum dma_data_direction dma_direction;
	unsigned int mapped_nents;
	unsigned int mapped_sg_len;
	size_t data_length;

	bool write_status_pending;
	bool hs_timing_seen;
	/* The clock profile the width transition targets and then reaches. */
	u32 target_clock_hz;
	enum ums9117_sdio_clock_profile target_clock_profile;
	bool app_cmd_armed;
	u32 app_cmd_arg;
	bool width_acmd6_clean;
	bool operational_clock_deferred;
	bool operational_clock_applied;
	bool width_switch_fatal;
	bool terminal_cleanup_hold;
	bool fatal_error;
	bool stopping;
	bool card_detect_enabled;
	struct ums9117_adi_transaction adi_transaction;
	bool audit_file_created;
	struct ta1618_mmc_audit audit;
};

static u32 ta1618_mmc_controller_read(void *context, enum ums9117_sdio_reg reg)
{
	struct ta1618_mmc_host *host = context;

	return readl(host->controller_regs[reg]);
}

static void ta1618_mmc_controller_write(void *context,
					enum ums9117_sdio_reg reg, u32 value)
{
	struct ta1618_mmc_host *host = context;

	writel(value, host->controller_regs[reg]);
}

static u32 ta1618_mmc_board_read(void *context, enum ta1618_sdio_board_reg reg)
{
	struct ta1618_mmc_host *host = context;

	return readl(host->board_regs[reg]);
}

static void ta1618_mmc_board_write(void *context,
				   enum ta1618_sdio_board_reg reg, u32 value)
{
	struct ta1618_mmc_host *host = context;

	writel(value, host->board_regs[reg]);
}

static u64 ta1618_mmc_time_us(void *context)
{
	(void)context;
	return ktime_to_us(ktime_get());
}

static void ta1618_mmc_delay_us(void *context, u32 usec)
{
	(void)context;
	udelay(usec);
}

static void ta1618_mmc_sleep_us(void *context, u32 minimum, u32 maximum)
{
	(void)context;
	if (minimum == maximum)
		maximum++;
	usleep_range(minimum, maximum);
}

static void ta1618_mmc_data_barrier(void *context)
{
	(void)context;
	dma_wmb();
}

static void ta1618_mmc_sleep_ms(void *context, u32 msec)
{
	(void)context;
	msleep(msec);
}

static int ta1618_mmc_adi_begin(void *context)
{
	struct ta1618_mmc_host *host = context;

	return ums9117_adi_begin(&host->adi_transaction);
}

static int ta1618_mmc_adi_read(void *context, enum ta1618_sdio_analog_reg reg,
			       u16 *value)
{
	struct ta1618_mmc_host *host = context;

	return ums9117_adi_read(&host->adi_transaction,
				ta1618_sdio_analog_offset(reg), value);
}

static int ta1618_mmc_adi_write(void *context, enum ta1618_sdio_analog_reg reg,
				u16 value)
{
	struct ta1618_mmc_host *host = context;

	return ums9117_adi_write(&host->adi_transaction,
				 ta1618_sdio_analog_offset(reg), value);
}

static int ta1618_mmc_adi_end(void *context)
{
	struct ta1618_mmc_host *host = context;

	return ums9117_adi_end(&host->adi_transaction);
}

static void ums9117_mmc_record_ios(struct ta1618_mmc_host *host,
				   const struct mmc_ios *ios, int result)
{
	struct ta1618_mmc_ios_trace_entry *entry;
	unsigned long flags;
	u32 sequence;

	spin_lock_irqsave(&host->lock, flags);
	sequence = ++host->audit.ios_calls;
	entry = &host->audit.ios[(sequence - 1U) % TA1618_MMC_IOS_TRACE_DEPTH];
	entry->sequence = sequence;
	entry->clock = ios->clock;
	entry->power_mode = ios->power_mode;
	entry->bus_width = ios->bus_width;
	entry->timing = ios->timing;
	entry->result = result;
	if (host->audit.ios_trace_count < TA1618_MMC_IOS_TRACE_DEPTH)
		host->audit.ios_trace_count++;
	spin_unlock_irqrestore(&host->lock, flags);
}

static void ums9117_mmc_audit_command_locked(struct ta1618_mmc_host *host,
					     const struct mmc_command *cmd,
					     bool width_acmd6)
{
	host->audit.last_command_valid = true;
	host->audit.last_opcode = cmd->opcode;
	host->audit.last_argument = cmd->arg;
	host->audit.last_lba_valid = false;
	host->audit.last_lba = 0;
	if (cmd->data) {
		host->audit.max_data_blocks =
			max(host->audit.max_data_blocks, cmd->data->blocks);
		host->audit.max_descriptors =
			max(host->audit.max_descriptors, host->mapped_nents);
	}
	switch (cmd->opcode) {
	case MMC_APP_CMD:
		host->audit.cmd55_attempt_count++;
		break;
	case SD_SWITCH:
		if (cmd->data && cmd->arg == UMS9117_MMC_CMD6_CHECK_ARG)
			host->audit.cmd6_check_count++;
		else if (cmd->data && cmd->arg == UMS9117_MMC_CMD6_SWITCH_ARG)
			host->audit.cmd6_switch_count++;
		break;
	case MMC_READ_SINGLE_BLOCK:
		host->audit.cmd17_count++;
		host->audit.last_cmd17_ordinal = host->audit.cmd17_count;
		if (host->mmc->card) {
			host->audit.last_lba =
				mmc_card_is_blockaddr(host->mmc->card) ?
					cmd->arg :
					cmd->arg >> 9;
			host->audit.last_lba_valid = true;
		}
		break;
	case MMC_WRITE_BLOCK:
		host->audit.cmd24_count++;
		break;
	case MMC_READ_MULTIPLE_BLOCK:
		host->audit.cmd18_count++;
		break;
	case MMC_WRITE_MULTIPLE_BLOCK:
		host->audit.cmd25_count++;
		break;
	default:
		break;
	}
	if (width_acmd6)
		host->audit.acmd6_attempt_count++;
}

static void ums9117_mmc_audit_irq_locked(struct ta1618_mmc_host *host,
					 u32 status, u32 owned,
					 u32 w1c_readback)
{
	host->audit.irq_count++;
	host->audit.last_irq_raw_status = status;
	host->audit.last_irq_owned_status = owned;
	host->audit.last_irq_w1c_readback = w1c_readback;
	if (status & UMS9117_SDIO_INT_ERROR)
		host->audit.irq_error_count++;
	if (status & (UMS9117_SDIO_INT_CRC | UMS9117_SDIO_INT_DATA_CRC))
		host->audit.irq_crc_count++;
	if (status & (UMS9117_SDIO_INT_END_BIT | UMS9117_SDIO_INT_DATA_END_BIT))
		host->audit.irq_end_bit_count++;
	if (status & (UMS9117_SDIO_INT_TIMEOUT | UMS9117_SDIO_INT_DATA_TIMEOUT))
		host->audit.irq_timeout_count++;
	if (status & UMS9117_SDIO_INT_TIMEOUT)
		host->audit.irq_command_timeout_count++;
	if (status & UMS9117_SDIO_INT_DATA_TIMEOUT)
		host->audit.irq_data_timeout_count++;
	if (status & UMS9117_SDIO_INT_AUTO_CMD12_ERROR)
		host->audit.irq_auto_cmd12_count++;
	if (status & UMS9117_SDIO_INT_ADMA_ERROR)
		host->audit.irq_adma_error_count++;
}

static bool ta1618_mmc_dma_address_valid(dma_addr_t address, size_t length)
{
	u64 start = (u64)address;

	return start && IS_ALIGNED(start, sizeof(u32)) && start <= U32_MAX &&
	       length && length - 1 <= U32_MAX - start;
}

static int ums9117_mmc_set_card_clock(struct ta1618_mmc_host *host, bool enable)
{
	struct ta1618_sdio_activation_record record;
	unsigned long flags;
	int ret;

	if (!enable)
		return ums9117_sdio_disable_card_clock(
			&host->sdio.controller, &host->sdio_state.controller);
	ret = ta1618_sdio_enable_ident_clock(&host->sdio, &host->sdio_state,
					     &record);
	if (ret)
		return ret;
	spin_lock_irqsave(&host->lock, flags);
	host->audit.selector_after_activate = record.selector_after;
	host->audit.clock_reset_after_activate = record.clock_after;
	host->audit.applied_clock_hz = UMS9117_SDIO_IDENT_CLOCK_HZ;
	host->audit.applied_clock_count++;
	spin_unlock_irqrestore(&host->lock, flags);
	return 0;
}

static int ta1618_mmc_activate_platform(struct ta1618_mmc_host *host)
{
	bool already_active = host->sdio_state.platform_active;
	unsigned long flags;
	u32 control;
	int ret;

	ret = ta1618_sdio_activate(&host->sdio, &host->sdio_state);
	if (ret || already_active)
		return ret;
	host->audit.host_control2_after_reset = ta1618_mmc_controller_read(
		host, UMS9117_SDIO_REG_HOST_CONTROL2);
	if (host->audit.host_control2_after_reset !=
	    UMS9117_SDIO_HOST_CTRL2_EXPECTED)
		dev_warn(host->dev,
			 "unexpected host control 2 after reset: 0x%08x\n",
			 host->audit.host_control2_after_reset);
	host->width_acmd6_clean = false;
	host->operational_clock_deferred = false;
	host->operational_clock_applied = false;
	host->terminal_cleanup_hold = false;
	host->app_cmd_armed = false;
	host->app_cmd_arg = 0;
	control = ta1618_mmc_controller_read(host,
					     UMS9117_SDIO_REG_HOST_CONTROL1);
	spin_lock_irqsave(&host->lock, flags);
	host->audit.host_ctrl1_width_before = control;
	host->audit.host_ctrl1_width_after = control;
	host->audit.selector_after_activate =
		ta1618_mmc_board_read(host, TA1618_SDIO_REG_CLOCK_SELECTOR);
	host->audit.clock_reset_after_activate =
		ta1618_mmc_controller_read(host, UMS9117_SDIO_REG_CLOCK_RESET);
	spin_unlock_irqrestore(&host->lock, flags);
	return 0;
}

static void ta1618_mmc_restore_platform(struct ta1618_mmc_host *host)
{
	int ret;

	ret = ta1618_sdio_restore_platform(
		&host->sdio, &host->sdio_state,
		UMS9117_MMC_READ_QUIESCE_DEADLINE_MS * 1000U);
	if (ret)
		dev_err(host->dev, "failed to restore SDIO0 baseline: %d\n",
			ret);
	host->width_acmd6_clean = false;
	host->operational_clock_deferred = false;
	host->operational_clock_applied = false;
}

static bool ta1618_mmc_is_destructive_command(const struct mmc_command *cmd)
{
	switch (cmd->opcode) {
	case MMC_WRITE_DAT_UNTIL_STOP:
	case MMC_SET_BLOCK_COUNT:
	case MMC_PROGRAM_CID:
	case MMC_PROGRAM_CSD:
	case MMC_SET_WRITE_PROT:
	case MMC_CLR_WRITE_PROT:
	case SD_ERASE_WR_BLK_START:
	case SD_ERASE_WR_BLK_END:
	case MMC_ERASE_GROUP_START:
	case MMC_ERASE_GROUP_END:
	case MMC_ERASE:
	case MMC_LOCK_UNLOCK:
		return true;
	case SD_SWITCH:
		/*
		 * Opcode 6 is shared: without data it is the bus-width
		 * application command, which must keep passing. With data it
		 * is switch-function, where only the query and the high-speed
		 * switch are recognised; anything else could write a card
		 * function group.
		 */
		return cmd->data && cmd->arg != UMS9117_MMC_CMD6_CHECK_ARG &&
		       cmd->arg != UMS9117_MMC_CMD6_SWITCH_ARG;
	default:
		return false;
	}
}

/*
 * The core builds exactly one shape for a multi-block transfer: CMD18 or CMD25
 * carrying the data, a bare CMD12 attached as the stop command, and no
 * set-block-count because this host never claims to support one. The stop
 * command waits for busy after a write and does not after a read, which is the
 * only difference between the two. Every field is checked rather than assumed,
 * so an unexpected shape is refused before any register is touched instead of
 * being transferred wrongly.
 */
static bool ums9117_mmc_is_multi_block(const struct mmc_request *mrq,
				       const struct mmc_host *mmc, bool write)
{
	const struct mmc_command *cmd = mrq->cmd;
	const struct mmc_command *stop = mrq->stop;
	const struct mmc_data *data = cmd->data;
	unsigned int opcode = write ? MMC_WRITE_MULTIPLE_BLOCK :
				      MMC_READ_MULTIPLE_BLOCK;
	unsigned int data_flag = write ? MMC_DATA_WRITE : MMC_DATA_READ;
	unsigned int stop_response = write ? MMC_RSP_R1B : MMC_RSP_R1;

	return cmd->opcode == opcode && data && data->flags == data_flag &&
	       data->blksz == 512 && data->blocks >= 2 &&
	       data->blocks <= mmc->max_blk_count && data->sg &&
	       data->sg_len >= 1 && data->sg_len <= mmc->max_segs &&
	       !mrq->sbc && stop && stop->opcode == MMC_STOP_TRANSMISSION &&
	       !stop->arg && !stop->data &&
	       mmc_resp_type(stop) == stop_response && !mrq->cap_cmd_during_tfr;
}

static int ta1618_mmc_response_type(const struct mmc_command *cmd,
				    enum ums9117_sdio_response_type *type)
{
	u32 response = mmc_resp_type(cmd);

	if (!response)
		*type = UMS9117_SDIO_RESPONSE_NONE;
	else if (response == MMC_RSP_R2)
		*type = UMS9117_SDIO_RESPONSE_LONG;
	else if (response == MMC_RSP_R1B)
		*type = UMS9117_SDIO_RESPONSE_SHORT_BUSY;
	else if (response == MMC_RSP_R3)
		*type = UMS9117_SDIO_RESPONSE_OCR;
	else if (response == MMC_RSP_R1 || response == MMC_RSP_R6 ||
		 response == MMC_RSP_R7)
		*type = UMS9117_SDIO_RESPONSE_SHORT;
	else
		return -EOPNOTSUPP;
	return 0;
}

static void ums9117_mmc_set_request_error(struct mmc_request *mrq, int error)
{
	mrq->cmd->error = error;
	if (mrq->cmd->data) {
		mrq->cmd->data->error = error;
		mrq->cmd->data->bytes_xfered = 0;
	}
}

static void ums9117_mmc_unmap_data(struct ta1618_mmc_host *host)
{
	struct mmc_data *data;

	if (!host->data_mapped)
		return;
	data = host->finish_mrq ? host->finish_mrq->cmd->data : NULL;
	if (!data && host->active_mrq)
		data = host->active_mrq->cmd->data;
	if (data)
		dma_unmap_sg(host->dev, data->sg, host->mapped_sg_len,
			     host->dma_direction);
	host->data_mapped = false;
	host->mapped_nents = 0;
	host->mapped_sg_len = 0;
	host->data_length = 0;
}

static int ums9117_mmc_prepare_data(struct ta1618_mmc_host *host,
				    struct mmc_command *cmd)
{
	struct mmc_data *data = cmd->data;
	struct scatterlist *sg;
	enum dma_data_direction direction;
	dma_addr_t address;
	size_t total_expected;
	size_t length;
	size_t total;
	int mapped;
	int i;

	if (data->flags == MMC_DATA_WRITE) {
		/*
		 * A multi-block write had every field checked before the
		 * request was admitted; only the single-block shape is
		 * constrained again here.
		 */
		if (cmd->opcode != MMC_WRITE_MULTIPLE_BLOCK &&
		    (cmd->opcode != MMC_WRITE_BLOCK || data->blocks != 1 ||
		     data->blksz != 512 || data->sg_len != 1))
			return -EOPNOTSUPP;
		direction = DMA_TO_DEVICE;
	} else if (cmd->opcode == MMC_READ_MULTIPLE_BLOCK) {
		/*
		 * Every field of a multi-block read was already checked before
		 * the request was admitted, so repeating the single-block
		 * shape guard here would only reject valid work.
		 */
		direction = DMA_FROM_DEVICE;
	} else {
		if (data->flags != MMC_DATA_READ || data->blocks != 1 ||
		    !data->blksz || data->blksz > 512 || data->sg_len != 1 ||
		    (cmd->opcode == MMC_READ_SINGLE_BLOCK &&
		     data->blksz != 512))
			return -EOPNOTSUPP;
		direction = DMA_FROM_DEVICE;
	}
	total_expected = (size_t)data->blocks * data->blksz;
	mapped = dma_map_sg(host->dev, data->sg, data->sg_len, direction);
	if (mapped <= 0)
		return mapped < 0 ? mapped : -EIO;
	if (mapped > (int)UMS9117_MMC_ADMA2_DESC_COUNT) {
		dma_unmap_sg(host->dev, data->sg, data->sg_len, direction);
		return -EOPNOTSUPP;
	}

	memset(host->descriptors, 0, UMS9117_MMC_ADMA2_TABLE_BYTES);
	total = 0;
	for_each_sg(data->sg, sg, mapped, i) {
		address = sg_dma_address(sg);
		length = sg_dma_len(sg);
		if (!length || length > host->mmc->max_seg_size ||
		    !ta1618_mmc_dma_address_valid(address, length)) {
			dma_unmap_sg(host->dev, data->sg, data->sg_len,
				     direction);
			return -ERANGE;
		}
		host->descriptors[i].attr = cpu_to_le16(
			i == mapped - 1 ? UMS9117_SDIO_ADMA2_TRANSFER_END :
					  UMS9117_SDIO_ADMA2_TRANSFER);
		host->descriptors[i].length = cpu_to_le16(length);
		host->descriptors[i].address = cpu_to_le32((u32)address);
		total += length;
	}
	/*
	 * The controller stops after block count times block size, so a
	 * scatterlist that does not add up to exactly that would silently
	 * transfer the wrong bytes rather than fail.
	 */
	if (total != total_expected) {
		dma_unmap_sg(host->dev, data->sg, data->sg_len, direction);
		return -EIO;
	}

	host->data_mapped = true;
	host->dma_direction = direction;
	host->mapped_nents = mapped;
	host->mapped_sg_len = data->sg_len;
	host->data_length = total;
	return 0;
}

static void ums9117_mmc_width_fail_closed(struct ta1618_mmc_host *host,
					  const char *reason, int error)
{
	unsigned long flags;
	u32 clock_before;
	u32 clock_after;
	u32 signal_after;
	bool cleanup_confirmed;

	/* Do not reset, re-clock, alter the divider, roll back ACMD6, or cycle rails. */
	ta1618_mmc_controller_write(
		host, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0);
	signal_after = ta1618_mmc_controller_read(
		host, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE);
	synchronize_irq(host->irq);
	clock_before =
		ta1618_mmc_controller_read(host, UMS9117_SDIO_REG_CLOCK_RESET);
	clock_after = clock_before & ~UMS9117_SDIO_CLOCK_CARD_EN;
	if (clock_after != clock_before)
		ta1618_mmc_controller_write(host, UMS9117_SDIO_REG_CLOCK_RESET,
					    clock_after);
	clock_after =
		ta1618_mmc_controller_read(host, UMS9117_SDIO_REG_CLOCK_RESET);
	cleanup_confirmed = !signal_after &&
			    !(clock_after & UMS9117_SDIO_CLOCK_CARD_EN);

	spin_lock_irqsave(&host->lock, flags);
	host->fatal_error = true;
	host->width_switch_fatal = true;
	host->terminal_cleanup_hold |= !cleanup_confirmed;
	host->app_cmd_armed = false;
	host->sdio_state.controller.card_clock_on =
		!!(clock_after & UMS9117_SDIO_CLOCK_CARD_EN);
	if (!host->sdio_state.controller.card_clock_on)
		host->sdio_state.controller.actual_clock_hz = 0;
	spin_unlock_irqrestore(&host->lock, flags);
	dev_crit(
		host->dev,
		"4-bit operational fail-closed: %s error=%d signal=0x%08x clock_before=0x%08x clock_after=0x%08x cleanup_confirmed=%u; IRQ masked, card clock off when confirmed, no reset/rail cycle/ACMD6 rollback/reclock; physical power-cycle required\n",
		reason, error, signal_after, clock_before, clock_after,
		cleanup_confirmed ? 1U : 0U);
}

static void ums9117_mmc_reject_request(struct ta1618_mmc_host *host,
				       struct mmc_request *mrq, int error,
				       const char *reason)
{
	/*
	 * Every caller runs before the first controller write, so neither the
	 * bus nor the card has moved and there is nothing to fail closed on.
	 * The core legitimately issues commands this host refuses, and each
	 * one used to cost the whole slot.
	 */
	dev_warn_ratelimited(
		host->dev,
		"request rejected before MMIO: %s; opcode=%u error=%d\n",
		reason, mrq->cmd->opcode, error);
	ums9117_mmc_set_request_error(mrq, error);
	mmc_request_done(host->mmc, mrq);
}

/*
 * Both operational frequencies share one recipe and differ only in the clock
 * divider. The choice is made by the timing the core asked for, never by the
 * frequency: a card without high speed asks for a number close to the fast one
 * and would otherwise be handed a bus it never agreed to.
 */
static bool ums9117_mmc_is_operational_clock(u32 hz)
{
	return hz >= UMS9117_SDIO_LEGACY_CLOCK_HZ &&
	       hz <= UMS9117_SDIO_HS_CLOCK_HZ;
}

static bool ums9117_mmc_select_clock_profile(struct ta1618_mmc_host *host,
					     const struct mmc_ios *ios)
{
	if (ios->timing == MMC_TIMING_SD_HS && host->hs_timing_seen &&
	    ios->clock >= UMS9117_SDIO_HS_CLOCK_HZ) {
		host->target_clock_hz = UMS9117_SDIO_HS_CLOCK_HZ;
		host->target_clock_profile = UMS9117_SDIO_CLOCK_HIGH_SPEED;
		return true;
	}
	/*
	 * A card that never agreed to high speed still asks for whatever its
	 * own description allows, which is usually just under the fast
	 * profile. A host is required to run at or below the frequency it was
	 * asked for, so that request is answered with the proven slow profile
	 * rather than refused.
	 */
	if (ios->clock >= UMS9117_SDIO_LEGACY_CLOCK_HZ) {
		host->target_clock_hz = UMS9117_SDIO_LEGACY_CLOCK_HZ;
		host->target_clock_profile = UMS9117_SDIO_CLOCK_LEGACY;
		return true;
	}
	return false;
}

static int ums9117_mmc_transition_width4(struct ta1618_mmc_host *host)
{
	struct ta1618_sdio_transition_record record;
	unsigned long flags;
	u32 clock_after;
	u32 control_after;
	int ret;

	if (!READ_ONCE(host->width_acmd6_clean) ||
	    READ_ONCE(host->sdio_state.controller.physical_width4) ||
	    !READ_ONCE(host->operational_clock_deferred))
		return -EPERM;

	spin_lock_irqsave(&host->lock, flags);
	if (host->active_mrq || host->finish_mrq) {
		ret = -EBUSY;
		goto out_unlock;
	}
	ret = ta1618_sdio_set_operational_clock(&host->sdio, &host->sdio_state,
						host->target_clock_profile,
						&record);
	clock_after = record.controller.clock_after ?
			      record.controller.clock_after :
			      record.controller.clock_before;
	control_after = record.controller.control_after ?
				record.controller.control_after :
				record.controller.control_before;
	host->audit.host_ctrl1_width_before = record.controller.control_before;
	host->audit.host_ctrl1_width_after = control_after;
	host->audit.selector_width_before = record.selector_before;
	host->audit.selector_width_after = record.selector_after;
	host->audit.clock_width_before = record.controller.clock_before;
	host->audit.clock_width_after = clock_after;
	host->audit.timeout_field_before =
		FIELD_GET(UMS9117_SDIO_CLOCK_TIMEOUT_MASK,
			  record.controller.clock_before);
	host->audit.timeout_register_before = record.controller.clock_before;
	host->audit.timeout_field_candidate =
		FIELD_GET(UMS9117_SDIO_CLOCK_TIMEOUT_MASK,
			  record.controller.clock_candidate);
	host->audit.timeout_register_candidate =
		record.controller.clock_candidate;
	host->audit.timeout_field_readback =
		FIELD_GET(UMS9117_SDIO_CLOCK_TIMEOUT_MASK,
			  record.controller.clock_readback);
	host->audit.timeout_register_readback =
		record.controller.clock_readback;
	if (!ret) {
		host->operational_clock_deferred = false;
		host->operational_clock_applied = true;
		host->audit.applied_clock_hz = host->target_clock_hz;
		host->audit.applied_clock_count++;
	}
out_unlock:
	spin_unlock_irqrestore(&host->lock, flags);
	return ret;
}

static int ums9117_mmc_response_error(const struct mmc_command *cmd,
				      u32 response)
{
	if (mmc_resp_type(cmd) != MMC_RSP_R1 &&
	    mmc_resp_type(cmd) != MMC_RSP_R1B)
		return 0;
	return ums9117_sdio_r1_error(response);
}

static unsigned int
ums9117_mmc_request_deadline_ms(const struct mmc_command *cmd, bool write)
{
	unsigned int base = write ? UMS9117_MMC_WRITE_REQUEST_TIMEOUT_MS :
				    UMS9117_MMC_REQUEST_TIMEOUT_MS;
	unsigned int per_block = write ? UMS9117_MMC_WRITE_BLOCK_BUDGET_MS :
					 UMS9117_MMC_READ_BLOCK_BUDGET_MS;

	if (!cmd->data)
		return base;
	return base + cmd->data->blocks * per_block;
}

static bool ums9117_mmc_is_write_data(const struct mmc_command *cmd)
{
	return cmd &&
	       (cmd->opcode == MMC_WRITE_BLOCK ||
		cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK) &&
	       cmd->data && cmd->data->flags == MMC_DATA_WRITE;
}

static bool ums9117_mmc_wait_quiescent(struct ta1618_mmc_host *host,
				       unsigned int deadline_ms,
				       u32 *last_present)
{
	return !ums9117_sdio_wait_quiescent(&host->sdio.controller,
					    deadline_ms * 1000U, last_present);
}

static void ums9117_mmc_finish_work(struct work_struct *work)
{
	struct ta1618_mmc_host *host =
		container_of(work, struct ta1618_mmc_host, finish_work);
	struct ums9117_sdio_completion completion = { 0 };
	struct mmc_request *mrq;
	struct mmc_command *cmd;
	unsigned long flags;
	u32 status;
	u32 response[4];
	bool ack_failed;
	bool needs_quiesce;
	bool width_acmd6;
	bool write_request;
	bool quiescent = true;
	u32 auto_cmd;
	u32 present = 0;
	u32 required_status = 0;
	unsigned int quiesce_deadline_ms;
	int error;

	cancel_delayed_work_sync(&host->timeout_work);
	spin_lock_irqsave(&host->lock, flags);
	mrq = host->finish_mrq;
	status = host->finish_status;
	memcpy(response, host->finish_response, sizeof(response));
	auto_cmd = host->finish_auto_cmd;
	ack_failed = host->finish_ack_failed;
	needs_quiesce = host->finish_needs_quiesce;
	width_acmd6 = host->finish_width_acmd6;
	spin_unlock_irqrestore(&host->lock, flags);
	if (!mrq)
		return;
	cmd = mrq->cmd;
	write_request = ums9117_mmc_is_write_data(cmd);
	quiesce_deadline_ms = write_request ?
				      UMS9117_MMC_WRITE_QUIESCE_DEADLINE_MS :
				      UMS9117_MMC_READ_QUIESCE_DEADLINE_MS;

	if (cmd->data || (cmd->flags & MMC_RSP_BUSY))
		required_status = UMS9117_SDIO_INT_RESPONSE |
				  UMS9117_SDIO_INT_TRANSFER;
	else if (cmd->flags & MMC_RSP_PRESENT)
		required_status = UMS9117_SDIO_INT_RESPONSE;
	completion.status = status;
	completion.owned_status = status & UMS9117_SDIO_STATUS_ENABLE_MASK;
	completion.status_readback = ack_failed ? completion.owned_status : 0;
	error = ums9117_sdio_validate_completion(&completion, required_status);
	if (!error)
		error = ums9117_mmc_response_error(cmd, response[0]);
	/*
	 * The controller issued the stop command on its own, so its outcome is
	 * reported only through the status bit and the automatic command error
	 * field. Both are consulted: a stop that failed leaves the card in the
	 * data state, and the core has to be told so it can recover.
	 */
	if (mrq->stop && ((status & UMS9117_SDIO_INT_AUTO_CMD12_ERROR) ||
			  (auto_cmd & UMS9117_SDIO_AUTO_CMD_ERROR_MASK))) {
		dev_err(host->dev,
			"automatic CMD12 failed: irq_status=0x%08x host_control2=0x%08x opcode=%u\n",
			status, auto_cmd, cmd->opcode);
		if (!error)
			error = -EIO;
	}
	memcpy(cmd->resp, response, sizeof(cmd->resp));
	if (needs_quiesce) {
		quiescent = ums9117_mmc_wait_quiescent(
			host, quiesce_deadline_ms, &present);
		if (!quiescent) {
			/*
			 * The transfer interrupt already arrived, so the
			 * controller is done with the buffer even though the
			 * data lines have not settled. Report the error and
			 * let the request complete: the MMC core has no way
			 * to abandon a request, so returning here would hang
			 * the block layer instead of protecting anything.
			 */
			dev_err(host->dev,
				"transfer quiescence not confirmed: opcode=%u irq_status=0x%08x present_state=0x%08x deadline_ms=%u\n",
				cmd->opcode, status, present,
				quiesce_deadline_ms);
			if (!error)
				error = -ETIMEDOUT;
		}
	}

	if (cmd->opcode == MMC_APP_CMD) {
		spin_lock_irqsave(&host->lock, flags);
		host->audit.cmd55_last_arg = cmd->arg;
		host->audit.cmd55_last_response = response[0];
		if (!error)
			host->audit.cmd55_clean_count++;
		if (!error && (response[0] & R1_APP_CMD)) {
			host->audit.cmd55_app_cmd_count++;
			host->app_cmd_armed = true;
			host->app_cmd_arg = cmd->arg;
		} else {
			host->app_cmd_armed = false;
			host->app_cmd_arg = 0;
		}
		spin_unlock_irqrestore(&host->lock, flags);
	}
	if (width_acmd6) {
		if (error) {
			spin_lock_irqsave(&host->lock, flags);
			host->audit.acmd6_failure_count++;
			spin_unlock_irqrestore(&host->lock, flags);
			ums9117_mmc_width_fail_closed(
				host, "admitted ACMD6 did not complete cleanly",
				error);
		} else {
			spin_lock_irqsave(&host->lock, flags);
			host->width_acmd6_clean = true;
			host->audit.acmd6_clean_count++;
			spin_unlock_irqrestore(&host->lock, flags);
		}
	}

	if (host->data_mapped)
		ums9117_mmc_unmap_data(host);
	if (error && (ums9117_mmc_is_write_data(cmd) ||
		      (READ_ONCE(host->write_status_pending) &&
		       cmd->opcode == MMC_SEND_STATUS))) {
		/*
		 * One failed write is not a reason to refuse every later
		 * request: the core retries a write on its own, and the
		 * status command below still has to run to clear the card.
		 */
		dev_err(host->dev, "write completion failed: %d\n", error);
	} else if (!error && ums9117_mmc_is_write_data(cmd)) {
		WRITE_ONCE(host->write_status_pending, true);
	} else if (!error && READ_ONCE(host->write_status_pending) &&
		   cmd->opcode == MMC_SEND_STATUS &&
		   mmc_ready_for_data(response[0])) {
		WRITE_ONCE(host->write_status_pending, false);
	}
	if (error)
		ums9117_mmc_set_request_error(mrq, error);
	else if (cmd->data) {
		cmd->data->error = 0;
		cmd->data->bytes_xfered =
			(unsigned int)cmd->data->blksz * cmd->data->blocks;
	}
	/*
	 * The stop command was never issued by this driver, so its response is
	 * left zero. The core only inspects it when the error is set, and a
	 * zero response trips none of its checks.
	 */
	if (mrq->stop)
		mrq->stop->error = error;
	cmd->error = error;

	spin_lock_irqsave(&host->lock, flags);
	host->finish_width_acmd6 = false;
	host->finish_mrq = NULL;
	spin_unlock_irqrestore(&host->lock, flags);
	mmc_request_done(host->mmc, mrq);
}

static int ums9117_mmc_recover(struct ta1618_mmc_host *host)
{
	bool restart_clock = host->sdio_state.rails_on &&
			     host->sdio_state.controller.card_clock_on;
	int ret;

	if (READ_ONCE(host->sdio_state.controller.physical_width4) ||
	    READ_ONCE(host->width_acmd6_clean))
		return -EPERM;
	ums9117_sdio_mask_and_ack(&host->sdio.controller);
	ret = ums9117_sdio_reset_controller(&host->sdio.controller);
	if (ret)
		return ret;
	ret = ums9117_sdio_configure_1bit(&host->sdio.controller,
					  &host->sdio_state.controller);
	if (!ret && restart_clock)
		ret = ums9117_mmc_set_card_clock(host, true);
	return ret;
}

static void ums9117_mmc_timeout_work(struct work_struct *work)
{
	struct ta1618_mmc_host *host = container_of(
		to_delayed_work(work), struct ta1618_mmc_host, timeout_work);
	struct mmc_request *mrq;
	unsigned long flags;
	bool post_write_status_timed_out;
	bool width_acmd6_timed_out;
	bool write_timed_out;
	bool write_related;
	bool last_lba_valid = false;
	u32 last_opcode = 0;
	u32 last_argument = 0;
	u32 last_lba = 0;
	u32 last_cmd17_ordinal = 0;
	u32 watchdog_count = 0;
	u32 present = 0;
	int ret;

	spin_lock_irqsave(&host->lock, flags);
	mrq = host->active_mrq;
	width_acmd6_timed_out = mrq && host->active_width_acmd6;
	write_timed_out = mrq && ums9117_mmc_is_write_data(mrq->cmd);
	post_write_status_timed_out = mrq &&
				      READ_ONCE(host->write_status_pending) &&
				      mrq->cmd->opcode == MMC_SEND_STATUS;
	write_related = write_timed_out || post_write_status_timed_out;
	if (mrq) {
		/* Win the IRQ race and retain this request until handling is safe. */
		host->audit.watchdog_timeout_count++;
		watchdog_count = host->audit.watchdog_timeout_count;
		last_opcode = host->audit.last_opcode;
		last_argument = host->audit.last_argument;
		last_lba = host->audit.last_lba;
		last_lba_valid = host->audit.last_lba_valid;
		last_cmd17_ordinal = host->audit.last_cmd17_ordinal;
		ta1618_mmc_controller_write(
			host, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0);
		host->active_mrq = NULL;
		host->finish_mrq = mrq;
		host->finish_width_acmd6 = host->active_width_acmd6;
		host->active_width_acmd6 = false;
	}
	spin_unlock_irqrestore(&host->lock, flags);
	if (!mrq)
		return;
	dev_err(host->dev,
		"software request watchdog fired: count=%u opcode=%u arg=0x%08x lba_valid=%u lba=%u cmd17_ordinal=%u deadline_ms=%u\n",
		watchdog_count, last_opcode, last_argument,
		last_lba_valid ? 1U : 0U, last_lba, last_cmd17_ordinal,
		mrq ? ums9117_mmc_request_deadline_ms(mrq->cmd, write_related) :
		      0U);
	synchronize_irq(host->irq);

	if (width_acmd6_timed_out) {
		spin_lock_irqsave(&host->lock, flags);
		host->audit.acmd6_failure_count++;
		spin_unlock_irqrestore(&host->lock, flags);
		ums9117_mmc_width_fail_closed(host, "admitted ACMD6 timed out",
					      -ETIMEDOUT);
		ums9117_mmc_set_request_error(mrq, -ETIMEDOUT);
		spin_lock_irqsave(&host->lock, flags);
		host->finish_width_acmd6 = false;
		host->finish_mrq = NULL;
		spin_unlock_irqrestore(&host->lock, flags);
		mmc_request_done(host->mmc, mrq);
		return;
	}

	if (write_related) {
		/*
		 * Only a bus that never settles justifies refusing all later
		 * I/O. When the lines are quiet the card and the controller
		 * are in a known state and the core recovers on its own, so
		 * one late write must not close the slot: that would also
		 * block the status and stop commands the recovery needs.
		 */
		if (!ums9117_mmc_wait_quiescent(
			    host, UMS9117_MMC_WRITE_QUIESCE_DEADLINE_MS,
			    &present)) {
			WRITE_ONCE(host->fatal_error, true);
			dev_err(host->dev,
				"write-related timeout without confirmed quiescence: present_state=0x%08x; blocking controller I/O until the slot is power-cycled\n",
				present);
		} else if (write_timed_out) {
			dev_err(host->dev,
				"write request timed out after admission; data lines quiescent, completing with an error and leaving the slot usable\n");
		} else {
			dev_err(host->dev,
				"post-write status command timed out; data lines quiescent, completing with an error and leaving the slot usable\n");
		}
		if (host->data_mapped)
			ums9117_mmc_unmap_data(host);
		ums9117_mmc_set_request_error(mrq, -ETIMEDOUT);
		spin_lock_irqsave(&host->lock, flags);
		host->finish_width_acmd6 = false;
		host->finish_mrq = NULL;
		spin_unlock_irqrestore(&host->lock, flags);
		mmc_request_done(host->mmc, mrq);
		return;
	}

	if (READ_ONCE(host->width_acmd6_clean)) {
		/*
		 * Only a bus that never settles justifies gating the card
		 * clock for good. If the lines are quiet the card and the
		 * controller are in a known state, so refusing further I/O
		 * until the core power-cycles the slot is enough.
		 */
		if (mrq->cmd->data &&
		    !ums9117_mmc_wait_quiescent(
			    host, UMS9117_MMC_WRITE_QUIESCE_DEADLINE_MS,
			    &present)) {
			ums9117_mmc_width_fail_closed(
				host, "timed-out transfer never quiesced",
				-ETIMEDOUT);
		} else {
			/*
			 * Quiet lines mean the card and the controller are in
			 * a known state. The core recovers a stalled transfer
			 * on its own with CMD13, then CMD12, and finally a
			 * power cycle of the slot, so one late transfer is no
			 * reason to refuse every later request as well.
			 *
			 * Forcing every write to time out shows the core
			 * recovers with or without this, so its value is
			 * consistency with the read path rather than a
			 * fault that was ever observed.
			 */
			dev_err(host->dev,
				"request timed out after accepted ACMD6; data lines quiescent, completing with an error and leaving the slot usable\n");
		}
	} else {
		mutex_lock(&host->state_mutex);
		ret = ums9117_mmc_recover(host);
		if (ret) {
			WRITE_ONCE(host->fatal_error, true);
			dev_err(host->dev,
				"request timeout recovery failed: %d\n", ret);
		}
		mutex_unlock(&host->state_mutex);
	}

	if (host->data_mapped)
		ums9117_mmc_unmap_data(host);
	ums9117_mmc_set_request_error(mrq, -ETIMEDOUT);
	spin_lock_irqsave(&host->lock, flags);
	host->finish_width_acmd6 = false;
	host->finish_mrq = NULL;
	spin_unlock_irqrestore(&host->lock, flags);
	mmc_request_done(host->mmc, mrq);
}

static irqreturn_t ums9117_mmc_irq(int irq, void *data)
{
	struct ta1618_mmc_host *host = data;
	struct ums9117_sdio_completion completion = { 0 };
	struct mmc_request *mrq;
	struct mmc_command *cmd;
	unsigned long flags;
	u32 status;
	u32 owned;
	u32 signal;
	bool terminal = false;

	if (irq != host->irq)
		return IRQ_NONE;
	status = ta1618_mmc_controller_read(host,
					    UMS9117_SDIO_REG_INTERRUPT_STATUS);
	owned = status & UMS9117_SDIO_STATUS_ENABLE_MASK;
	if (!owned)
		return IRQ_NONE;

	spin_lock_irqsave(&host->lock, flags);
	mrq = host->active_mrq;
	cmd = mrq ? mrq->cmd : NULL;
	if (cmd) {
		u32 terminal_bit = cmd->data || (cmd->flags & MMC_RSP_BUSY) ?
					   UMS9117_SDIO_INT_TRANSFER :
					   UMS9117_SDIO_INT_RESPONSE;

		terminal = ums9117_sdio_status_terminal(status, terminal_bit);
	}
	spin_unlock_irqrestore(&host->lock, flags);

	if (terminal) {
		ums9117_sdio_capture_completion(
			&host->sdio.controller, status,
			!!(cmd->flags & MMC_RSP_136), true,
			UMS9117_SDIO_ACK_BEFORE_RESPONSE, &completion);
	} else {
		ta1618_mmc_controller_write(
			host, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0);
		completion.status = status;
		completion.owned_status = owned;
		completion.auto_command_status = ta1618_mmc_controller_read(
			host, UMS9117_SDIO_REG_HOST_CONTROL2);
		ta1618_mmc_controller_write(
			host, UMS9117_SDIO_REG_INTERRUPT_STATUS, owned);
		completion.status_readback = ta1618_mmc_controller_read(
			host, UMS9117_SDIO_REG_INTERRUPT_STATUS);
	}

	spin_lock_irqsave(&host->lock, flags);
	ums9117_mmc_audit_irq_locked(host, status, owned,
				     completion.status_readback);
	if (status & (UMS9117_SDIO_INT_TIMEOUT | UMS9117_SDIO_INT_DATA_TIMEOUT))
		dev_err(host->dev,
			"hardware timeout IRQ: command=%u data=%u raw=0x%08x owned=0x%08x w1c_readback=0x%08x opcode=%u arg=0x%08x lba_valid=%u lba=%u cmd17_ordinal=%u\n",
			(status & UMS9117_SDIO_INT_TIMEOUT) ? 1U : 0U,
			(status & UMS9117_SDIO_INT_DATA_TIMEOUT) ? 1U : 0U,
			status, owned, completion.status_readback,
			host->audit.last_opcode, host->audit.last_argument,
			host->audit.last_lba_valid ? 1U : 0U,
			host->audit.last_lba, host->audit.last_cmd17_ordinal);
	mrq = host->active_mrq;
	if (!mrq) {
		spin_unlock_irqrestore(&host->lock, flags);
		return IRQ_HANDLED;
	}
	cmd = mrq->cmd;
	if (!terminal) {
		signal = cmd->data || (cmd->flags & MMC_RSP_BUSY) ?
				 UMS9117_MMC_SIGNAL_DATA :
				 UMS9117_MMC_SIGNAL_COMMAND;
		ta1618_mmc_controller_write(
			host, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, signal);
		spin_unlock_irqrestore(&host->lock, flags);
		return IRQ_HANDLED;
	}

	memcpy(host->finish_response, completion.response,
	       sizeof(host->finish_response));
	host->finish_status = completion.status;
	host->finish_auto_cmd = completion.auto_command_status;
	host->finish_ack_failed =
		!!(completion.status_readback & completion.owned_status);
	host->finish_needs_quiesce = cmd->data || (cmd->flags & MMC_RSP_BUSY);
	host->finish_width_acmd6 = host->active_width_acmd6;
	host->active_width_acmd6 = false;
	host->finish_mrq = mrq;
	host->active_mrq = NULL;
	spin_unlock_irqrestore(&host->lock, flags);

	cancel_delayed_work(&host->timeout_work);
	schedule_work(&host->finish_work);
	return IRQ_HANDLED;
}

static void ums9117_mmc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct ta1618_mmc_host *host = mmc_priv(mmc);
	struct mmc_command *cmd = mrq->cmd;
	struct mmc_data *data = cmd->data;
	struct ums9117_sdio_data_setup setup;
	const struct ums9117_sdio_data_setup *setup_ptr;
	unsigned long flags;
	u32 signal;
	u16 command_flags;
	u16 command;
	u16 transfer = 0;
	enum ums9117_sdio_response_type response_type;
	u32 app_cmd_arg = 0;
	bool app_context = false;
	bool cmd24_write;
	bool multi_read;
	bool multi_write;
	bool write_request;
	bool post_write_status;
	bool width_acmd6;
	int ret;

	if (READ_ONCE(host->width_switch_fatal)) {
		ums9117_mmc_set_request_error(mrq, -EIO);
		mmc_request_done(mmc, mrq);
		return;
	}
	spin_lock_irqsave(&host->lock, flags);
	if (cmd->opcode != MMC_APP_CMD) {
		app_context = host->app_cmd_armed;
		app_cmd_arg = host->app_cmd_arg;
		host->app_cmd_armed = false;
		host->app_cmd_arg = 0;
	}
	spin_unlock_irqrestore(&host->lock, flags);
	width_acmd6 = app_context && (app_cmd_arg & 0xffff0000U) &&
		      !(app_cmd_arg & 0x0000ffffU) &&
		      cmd->opcode == SD_APP_SET_BUS_WIDTH && !data &&
		      cmd->arg == SD_BUS_WIDTH_4;
	if ((cmd->opcode == SD_APP_SET_BUS_WIDTH && !data && !width_acmd6) ||
	    (width_acmd6 &&
	     (READ_ONCE(host->sdio_state.controller.physical_width4) ||
	      READ_ONCE(host->width_acmd6_clean) ||
	      !READ_ONCE(host->operational_clock_deferred) ||
	      READ_ONCE(host->sdio_state.controller.actual_clock_hz) !=
		      UMS9117_SDIO_IDENT_CLOCK_HZ ||
	      !READ_ONCE(host->sdio_state.controller.card_clock_on)))) {
		ums9117_mmc_reject_request(
			host, mrq, -EPROTO,
			"application-command qualification rejected request");
		return;
	}
	post_write_status = READ_ONCE(host->write_status_pending) &&
			    cmd->opcode == MMC_SEND_STATUS && !data;
	if (READ_ONCE(host->write_status_pending) && !post_write_status) {
		WRITE_ONCE(host->fatal_error, true);
		ums9117_mmc_reject_request(
			host, mrq, -EIO,
			"post-CMD24 status sequencing rejected request");
		return;
	}
	cmd24_write = cmd->opcode == MMC_WRITE_BLOCK && data &&
		      data->flags == MMC_DATA_WRITE && data->blocks == 1 &&
		      data->blksz == 512 && data->sg_len == 1;
	multi_read = ums9117_mmc_is_multi_block(mrq, mmc, false);
	multi_write = ums9117_mmc_is_multi_block(mrq, mmc, true);
	write_request = cmd24_write || multi_write;
	if ((cmd->opcode == MMC_READ_SINGLE_BLOCK ||
	     cmd->opcode == MMC_READ_MULTIPLE_BLOCK ||
	     cmd->opcode == MMC_WRITE_BLOCK ||
	     cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK) &&
	    (!READ_ONCE(host->sdio_state.controller.physical_width4) ||
	     !READ_ONCE(host->operational_clock_applied) ||
	     !ums9117_mmc_is_operational_clock(
		     READ_ONCE(host->sdio_state.controller.actual_clock_hz)))) {
		ums9117_mmc_reject_request(
			host, mrq, -EPROTO,
			"block I/O requested before a physical 4-bit operational state");
		return;
	}
	if (ta1618_mmc_is_destructive_command(cmd) ||
	    (cmd->opcode == MMC_WRITE_BLOCK && !cmd24_write) ||
	    (cmd->opcode == MMC_READ_MULTIPLE_BLOCK && !multi_read) ||
	    (cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK && !multi_write) ||
	    (data && (data->flags & MMC_DATA_WRITE) && !write_request)) {
		ums9117_mmc_reject_request(
			host, mrq, -EOPNOTSUPP,
			"forbidden data or destructive opcode rejected before MMIO");
		return;
	}
	if (mrq->sbc || (mrq->stop && !multi_read && !multi_write) ||
	    cmd->opcode > 63U) {
		ums9117_mmc_reject_request(
			host, mrq, -EOPNOTSUPP,
			"SBC, STOP, or invalid opcode rejected before MMIO");
		return;
	}
	ret = ta1618_mmc_response_type(cmd, &response_type);
	if (!ret)
		ret = ums9117_sdio_response_flags(response_type,
						  &command_flags);
	if (ret) {
		ums9117_mmc_reject_request(
			host, mrq, ret,
			"unsupported response type rejected before MMIO");
		return;
	}
	if (data) {
		ret = ums9117_mmc_prepare_data(host, cmd);
		if (ret) {
			ums9117_mmc_reject_request(
				host, mrq, ret,
				"data mapping or shape rejected before MMIO");
			return;
		}
		if (multi_read)
			transfer = UMS9117_SDIO_TRANSFER_CMD18_AUTO_CMD12_ADMA2;
		else if (multi_write)
			transfer = UMS9117_SDIO_TRANSFER_CMD25_AUTO_CMD12_ADMA2;
		else
			transfer = data->flags == MMC_DATA_WRITE ?
					   UMS9117_SDIO_TRANSFER_WRITE_ADMA2 :
					   UMS9117_SDIO_TRANSFER_READ_ADMA2;
	}

	if (READ_ONCE(host->fatal_error) || READ_ONCE(host->stopping) ||
	    !READ_ONCE(host->sdio_state.rails_on) ||
	    !READ_ONCE(host->sdio_state.controller.card_clock_on)) {
		ret = -EIO;
		goto out_error;
	}
	ret = ums9117_sdio_wait_inhibit(&host->sdio.controller,
					data || (cmd->flags & MMC_RSP_BUSY));
	if (ret)
		goto out_error;

	spin_lock_irqsave(&host->lock, flags);
	if (host->stopping || host->active_mrq || host->finish_mrq) {
		ret = host->stopping ? -ESHUTDOWN : -EBUSY;
		spin_unlock_irqrestore(&host->lock, flags);
		goto out_error;
	}
	ret = ta1618_sdio_validate_active(&host->sdio, &host->sdio_state);
	if (ret) {
		spin_unlock_irqrestore(&host->lock, flags);
		goto out_error;
	}
	if ((multi_read || multi_write) &&
	    ta1618_mmc_controller_read(host, UMS9117_SDIO_REG_HOST_CONTROL2) !=
		    UMS9117_SDIO_HOST_CTRL2_EXPECTED) {
		spin_unlock_irqrestore(&host->lock, flags);
		ret = -EPROTO;
		goto out_error;
	}
	if (data) {
		setup.blocks = data->blocks;
		setup.block_size = data->blksz;
		setup.adma_address = (u32)host->descriptors_dma;
		setup_ptr = &setup;
		command_flags |= UMS9117_SDIO_CMD_DATA;
		signal = UMS9117_MMC_SIGNAL_DATA;
	} else {
		setup_ptr = NULL;
		command_flags |= UMS9117_SDIO_SUB_CMD;
		signal = cmd->flags & MMC_RSP_BUSY ? UMS9117_MMC_SIGNAL_DATA :
						     UMS9117_MMC_SIGNAL_COMMAND;
	}
	ret = ums9117_sdio_prepare_request(&host->sdio.controller, setup_ptr);
	if (ret || (width_acmd6 &&
		    ta1618_mmc_controller_read(
			    host, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE))) {
		spin_unlock_irqrestore(&host->lock, flags);
		if (!ret)
			ret = -EIO;
		goto out_error;
	}

	command = (cmd->opcode << 8) | command_flags;
	host->active_mrq = mrq;
	host->active_width_acmd6 = width_acmd6;
	ums9117_mmc_audit_command_locked(host, cmd, width_acmd6);
	schedule_delayed_work(&host->timeout_work,
			      msecs_to_jiffies(ums9117_mmc_request_deadline_ms(
				      cmd, write_request)));
	ums9117_sdio_issue_request(&host->sdio.controller, cmd->arg, command,
				   data ? transfer : 0U, signal);
	spin_unlock_irqrestore(&host->lock, flags);
	return;

out_error:
	if (host->data_mapped) {
		host->active_mrq = mrq;
		ums9117_mmc_unmap_data(host);
		host->active_mrq = NULL;
	}
	ums9117_mmc_set_request_error(mrq, ret);
	mmc_request_done(mmc, mrq);
}

static int ums9117_mmc_set_host_1bit_powered_off(struct ta1618_mmc_host *host)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&host->lock, flags);
	if (host->active_mrq || host->finish_mrq) {
		ret = -EBUSY;
		goto out_unlock;
	}
	ret = ums9117_sdio_set_1bit(&host->sdio.controller,
				    &host->sdio_state.controller);
	if (ret)
		goto out_unlock;
	host->width_acmd6_clean = false;
	host->operational_clock_deferred = false;
	host->operational_clock_applied = false;
	host->app_cmd_armed = false;
	host->app_cmd_arg = 0;
out_unlock:
	spin_unlock_irqrestore(&host->lock, flags);
	return ret;
}

static void ums9117_mmc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ta1618_mmc_host *host = mmc_priv(mmc);
	unsigned long flags;
	int ret = 0;

	mutex_lock(&host->state_mutex);
	spin_lock_irqsave(&host->lock, flags);
	host->audit.requested_clock_hz = ios->clock;
	spin_unlock_irqrestore(&host->lock, flags);
	if (host->stopping)
		goto out_trace;
	if (READ_ONCE(host->width_switch_fatal) &&
	    ios->power_mode != MMC_POWER_OFF) {
		ret = -ESHUTDOWN;
		dev_err(host->dev,
			"set_ios refused after the 4-bit fail-closed; only a power cycle clears it\n");
		goto out_trace;
	}
	/*
	 * The core only asks for this timing after the card answered the
	 * switch cleanly, so seeing it is the confirmation that the card is
	 * in high speed. The driver never parses the switch payload itself.
	 */
	if (ios->timing == MMC_TIMING_SD_HS)
		host->hs_timing_seen = true;
	if ((ios->bus_width != MMC_BUS_WIDTH_1 &&
	     ios->bus_width != MMC_BUS_WIDTH_4) ||
	    (ios->timing != MMC_TIMING_LEGACY &&
	     ios->timing != MMC_TIMING_SD_HS) ||
	    (ios->clock && !ums9117_mmc_is_operational_clock(ios->clock) &&
	     (ios->clock < UMS9117_SDIO_IDENT_CLOCK_HZ ||
	      ios->clock > UMS9117_MMC_IDENT_REQUEST_MAX_HZ))) {
		ret = -EOPNOTSUPP;
		goto out_fail;
	}

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		if (host->sdio_state.platform_active) {
			ums9117_sdio_mask_and_ack(&host->sdio.controller);
			ret = ums9117_mmc_set_card_clock(host, false);
			if (!ret)
				ret = ta1618_sdio_set_slot_power(
					&host->sdio, &host->sdio_state, false);
			if (!ret)
				ret = ums9117_mmc_set_host_1bit_powered_off(
					host);
		}
		if (!ret) {
			/*
			 * The rails are down and the host is back in 1-bit
			 * ADMA2, so the card and the controller can no longer
			 * disagree about the bus width. That is the whole
			 * reason the latches existed; clearing them here is
			 * what lets the core recover a slot by power-cycling
			 * it instead of needing the battery pulled.
			 *
			 * The outstanding post-write status obligation goes
			 * with them. A card that lost its rails is no longer
			 * programming anything, and carrying the obligation
			 * across would reject the very first command of the
			 * next identification and deadlock the slot.
			 *
			 * Observed: making the post-write status command
			 * fail once, with this line removed, leaves CMD0,
			 * CMD8 and CMD55 refused and the card never
			 * returns. With it, the card is re-identified and
			 * I/O resumes.
			 */
			spin_lock_irqsave(&host->lock, flags);
			host->fatal_error = false;
			host->width_switch_fatal = false;
			host->terminal_cleanup_hold = false;
			host->hs_timing_seen = false;
			host->write_status_pending = false;
			spin_unlock_irqrestore(&host->lock, flags);
		}
		break;
	case MMC_POWER_UP:
		if (ios->bus_width != MMC_BUS_WIDTH_1) {
			ret = -EPROTO;
			break;
		}
		ret = ta1618_mmc_activate_platform(host);
		break;
	case MMC_POWER_ON:
		if (!host->sdio_state.platform_active ||
		    !host->sdio_state.rails_on) {
			ret = -EIO;
			break;
		}
		if (ios->bus_width == MMC_BUS_WIDTH_1) {
			if (host->width_acmd6_clean ||
			    host->sdio_state.controller.physical_width4 ||
			    host->operational_clock_applied) {
				ret = -EPROTO;
				break;
			}
			if (ios->clock &&
			    !host->sdio_state.controller.card_clock_on)
				ret = ums9117_mmc_set_card_clock(host, true);
			else if (!ios->clock &&
				 host->sdio_state.controller.card_clock_on)
				ret = ums9117_mmc_set_card_clock(host, false);
			if (ret)
				break;
			if (ios->clock &&
			    host->sdio_state.controller.actual_clock_hz !=
				    UMS9117_SDIO_IDENT_CLOCK_HZ) {
				ret = -EPROTO;
				break;
			}
			if (ums9117_mmc_is_operational_clock(ios->clock)) {
				host->operational_clock_deferred = true;
				spin_lock_irqsave(&host->lock, flags);
				host->audit.deferred_clock_hz = ios->clock;
				host->audit.deferred_clock_count++;
				spin_unlock_irqrestore(&host->lock, flags);
			} else {
				host->operational_clock_deferred = false;
			}
		} else {
			if (!host->width_acmd6_clean ||
			    !host->sdio_state.controller.card_clock_on ||
			    !ums9117_mmc_select_clock_profile(host, ios)) {
				ret = -EPROTO;
				break;
			}
			if (!host->sdio_state.controller.physical_width4)
				ret = ums9117_mmc_transition_width4(host);
			else if (!host->operational_clock_applied ||
				 host->sdio_state.controller.actual_clock_hz !=
					 host->target_clock_hz)
				ret = -EPROTO;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret)
		goto out_trace;

out_fail:
	if (READ_ONCE(host->width_acmd6_clean)) {
		ums9117_mmc_width_fail_closed(
			host, "set_ios failed after accepted ACMD6", ret);
		goto out_trace;
	}
	host->fatal_error = true;
	dev_err(host->dev,
		"set_ios failed: power=%u clock=%u width=%u timing=%u actual_clock=%u error=%d\n",
		ios->power_mode, ios->clock, ios->bus_width, ios->timing,
		host->sdio_state.controller.actual_clock_hz, ret);
	ta1618_mmc_restore_platform(host);
out_trace:
	ums9117_mmc_record_ios(host, ios, ret);
	mutex_unlock(&host->state_mutex);
}

static int ums9117_mmc_get_cd(struct mmc_host *mmc)
{
	struct ta1618_mmc_host *host = mmc_priv(mmc);
	int present;

	if (!READ_ONCE(host->card_detect_enabled)) {
		dev_err_ratelimited(
			host->dev,
			"card-detect is not enabled; reporting card absent\n");
		return 0;
	}

	present = ta1618_sdio_card_present(&host->sdio, &host->sdio_state);
	if (present < 0) {
		dev_err_ratelimited(
			host->dev,
			"card-detect ownership lost: %d; reporting card absent\n",
			present);
		return 0;
	}
	return present;
}

static int ums9117_mmc_get_ro(struct mmc_host *mmc)
{
	/*
	 * The slot has no write-protect input to read. A card that is
	 * physically locked still stays read-only, because the core also
	 * requires the block-write command class from the card itself.
	 */
	(void)mmc;
	return 0;
}

static int ums9117_mmc_multi_io_quirk(struct mmc_card *card,
				      unsigned int direction, int blk_size)
{
	/*
	 * The core takes this answer as the block count for the request, so
	 * returning one is what would confine every transfer to a single
	 * block. Both directions run multi-block, bounded by the host limit.
	 */
	(void)direction;
	return min_t(int, blk_size, card->host->max_blk_count);
}

static ssize_t audit_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ta1618_mmc_host *host = dev_get_drvdata(dev);
	struct ta1618_mmc_audit audit;
	unsigned long flags;
	bool active;
	bool app_cmd_armed;
	bool fatal;
	bool finish;
	bool operational_clock_applied;
	bool operational_clock_deferred;
	bool physical_width4;
	bool terminal_cleanup_hold;
	bool width_acmd6_clean;
	bool width_switch_fatal;
	bool hs_timing_seen;
	u32 actual_clock_hz;
	u32 target_divider;
	u32 target_operational;
	unsigned int i;
	ssize_t len = 0;

	(void)attr;
	spin_lock_irqsave(&host->lock, flags);
	audit = host->audit;
	active = host->active_mrq != NULL;
	finish = host->finish_mrq != NULL;
	app_cmd_armed = host->app_cmd_armed;
	width_acmd6_clean = host->width_acmd6_clean;
	physical_width4 = host->sdio_state.controller.physical_width4;
	operational_clock_deferred = host->operational_clock_deferred;
	operational_clock_applied = host->operational_clock_applied;
	actual_clock_hz = host->sdio_state.controller.actual_clock_hz;
	fatal = host->fatal_error;
	width_switch_fatal = host->width_switch_fatal;
	hs_timing_seen = host->hs_timing_seen;
	target_divider = host->target_clock_profile ==
					 UMS9117_SDIO_CLOCK_HIGH_SPEED ?
				 UMS9117_SDIO_HS_DIVIDER :
				 UMS9117_SDIO_LEGACY_DIVIDER;
	target_operational = host->target_clock_profile ==
					     UMS9117_SDIO_CLOCK_HIGH_SPEED ?
				     UMS9117_SDIO_HS_CLOCK_EXPECTED :
				     UMS9117_SDIO_LEGACY_CLOCK_EXPECTED;
	terminal_cleanup_hold = host->terminal_cleanup_hold;
	spin_unlock_irqrestore(&host->lock, flags);

	len += sysfs_emit_at(
		buf, len,
		"profile=sd-perf-4bit identification_width=1 read_only=0 caps_4bit=1 caps_highspeed=1 caps_uhs=0 caps_cmd23=0 caps_8bit=0 voltage_switch=0 tuning=0\n");
	len += sysfs_emit_at(
		buf, len,
		"clock_ident=selector:0x%08x,divider:0x%08x,actual_hz:%u,clock_reset:0x%08x\n",
		audit.selector_after_activate, UMS9117_SDIO_IDENT_DIVIDER,
		UMS9117_SDIO_IDENT_CLOCK_HZ, audit.clock_reset_after_activate);
	len += sysfs_emit_at(
		buf, len,
		"clock_state=requested_hz:%u,deferred_hz:%u,deferred_count:%u,applied_hz:%u,applied_count:%u,actual_hz:%u,deferred:%u,applied:%u,legacy_hz:%u,highspeed_hz:%u,target_divider:0x%08x,target_word:0x%08x\n",
		audit.requested_clock_hz, audit.deferred_clock_hz,
		audit.deferred_clock_count, audit.applied_clock_hz,
		audit.applied_clock_count, actual_clock_hz,
		operational_clock_deferred ? 1U : 0U,
		operational_clock_applied ? 1U : 0U,
		UMS9117_SDIO_LEGACY_CLOCK_HZ, UMS9117_SDIO_HS_CLOCK_HZ,
		target_divider, target_operational);
	len += sysfs_emit_at(
		buf, len,
		"cmd55=attempts:%u,clean:%u,app_cmd:%u,last_arg:0x%08x,last_response:0x%08x,armed:%u\n",
		audit.cmd55_attempt_count, audit.cmd55_clean_count,
		audit.cmd55_app_cmd_count, audit.cmd55_last_arg,
		audit.cmd55_last_response, app_cmd_armed ? 1U : 0U);
	len += sysfs_emit_at(
		buf, len,
		"acmd6=attempts:%u,clean:%u,failures:%u,arg:0x%08x,host_generated:0\n",
		audit.acmd6_attempt_count, audit.acmd6_clean_count,
		audit.acmd6_failure_count, SD_BUS_WIDTH_4);
	len += sysfs_emit_at(
		buf, len,
		"width=acmd6_clean:%u,physical_4bit:%u,host_ctrl1_before:0x%08x,host_ctrl1_after:0x%08x,owned_mask:0x%08x,owned_1bit:0x%08x,owned_4bit:0x%08x\n",
		width_acmd6_clean ? 1U : 0U, physical_width4 ? 1U : 0U,
		audit.host_ctrl1_width_before, audit.host_ctrl1_width_after,
		(u32)UMS9117_SDIO_HOST_CTRL1_OWNED_MASK,
		UMS9117_SDIO_HOST_CTRL1_1BIT_ADMA2,
		UMS9117_SDIO_HOST_CTRL1_4BIT_ADMA2);
	len += sysfs_emit_at(
		buf, len,
		"width_clock=selector_before:0x%08x,selector_after:0x%08x,clock_before:0x%08x,clock_after:0x%08x\n",
		audit.selector_width_before, audit.selector_width_after,
		audit.clock_width_before, audit.clock_width_after);
	len += sysfs_emit_at(
		buf, len,
		"timeout_program=mask:0x%08x,fpdoom_value:%u,before_field:%u,before_register:0x%08x,candidate_field:%u,candidate_register:0x%08x,readback_field:%u,readback_register:0x%08x,operational_expected:0x%08x,operational_readback:0x%08x\n",
		(u32)UMS9117_SDIO_CLOCK_TIMEOUT_MASK,
		FIELD_GET(UMS9117_SDIO_CLOCK_TIMEOUT_MASK,
			  UMS9117_SDIO_TIMEOUT_ENCODED),
		audit.timeout_field_before, audit.timeout_register_before,
		audit.timeout_field_candidate, audit.timeout_register_candidate,
		audit.timeout_field_readback, audit.timeout_register_readback,
		target_operational, audit.clock_width_after);
	len += sysfs_emit_at(
		buf, len,
		"commands=cmd17:%u,cmd24:%u,cmd18:%u,cmd25:%u,max_blocks:%u\n",
		audit.cmd17_count, audit.cmd24_count, audit.cmd18_count,
		audit.cmd25_count, audit.max_data_blocks);
	len += sysfs_emit_at(
		buf, len,
		"highspeed=caps:%u,timing_seen:%u,cmd6_check:%u,cmd6_switch:%u,max_hs_hz:%u\n",
		1U, hs_timing_seen ? 1U : 0U, audit.cmd6_check_count,
		audit.cmd6_switch_count, UMS9117_SDIO_HS_CLOCK_HZ);
	len += sysfs_emit_at(buf, len,
			     "descriptors=table:%u,capacity:%u,max_used:%u\n",
			     (u32)UMS9117_MMC_ADMA2_TABLE_BYTES,
			     (u32)UMS9117_MMC_ADMA2_DESC_COUNT,
			     audit.max_descriptors);
	len += sysfs_emit_at(
		buf, len,
		"last_command=valid:%u,opcode:%u,arg:0x%08x,lba_valid:%u,lba:%u,cmd17_ordinal:%u\n",
		audit.last_command_valid ? 1U : 0U, audit.last_opcode,
		audit.last_argument, audit.last_lba_valid ? 1U : 0U,
		audit.last_lba, audit.last_cmd17_ordinal);
	len += sysfs_emit_at(
		buf, len,
		"irq=count:%u,error:%u,crc:%u,end_bit:%u,timeout:%u,adma:%u,auto_cmd12:%u\n",
		audit.irq_count, audit.irq_error_count, audit.irq_crc_count,
		audit.irq_end_bit_count, audit.irq_timeout_count,
		audit.irq_adma_error_count, audit.irq_auto_cmd12_count);
	len += sysfs_emit_at(
		buf, len, "host_control2=after_reset:0x%08x,expected:0x%08x\n",
		audit.host_control2_after_reset,
		UMS9117_SDIO_HOST_CTRL2_EXPECTED);
	len += sysfs_emit_at(
		buf, len,
		"timeouts=command_irq:%u,command_mask:0x%08x,data_irq:%u,data_mask:0x%08x,software_watchdog:%u\n",
		audit.irq_command_timeout_count, UMS9117_SDIO_INT_TIMEOUT,
		audit.irq_data_timeout_count, UMS9117_SDIO_INT_DATA_TIMEOUT,
		audit.watchdog_timeout_count);
	len += sysfs_emit_at(
		buf, len,
		"irq_last=raw_status:0x%08x,owned_status:0x%08x,w1c_readback:0x%08x\n",
		audit.last_irq_raw_status, audit.last_irq_owned_status,
		audit.last_irq_w1c_readback);
	len += sysfs_emit_at(
		buf, len,
		"state=active:%u,finish:%u,fatal:%u,width_fail_closed:%u,terminal_cleanup_hold:%u\n",
		active ? 1U : 0U, finish ? 1U : 0U, fatal ? 1U : 0U,
		width_switch_fatal ? 1U : 0U, terminal_cleanup_hold ? 1U : 0U);
	len += sysfs_emit_at(buf, len, "ios_trace=count:%u,total:%u\n",
			     audit.ios_trace_count, audit.ios_calls);
	for (i = 0; i < TA1618_MMC_IOS_TRACE_DEPTH && len < PAGE_SIZE; i++) {
		const struct ta1618_mmc_ios_trace_entry *entry = &audit.ios[i];

		if (!entry->sequence)
			continue;
		len += sysfs_emit_at(
			buf, len,
			"ios[%u]=sequence:%u,power:%u,clock:%u,width:%u,timing:%u,result:%d\n",
			i, entry->sequence, entry->power_mode, entry->clock,
			entry->bus_width, entry->timing, entry->result);
	}
	return len;
}
static DEVICE_ATTR_RO(audit);

static const struct mmc_host_ops ums9117_mmc_ops = {
	.request = ums9117_mmc_request,
	.set_ios = ums9117_mmc_set_ios,
	.get_cd = ums9117_mmc_get_cd,
	.get_ro = ums9117_mmc_get_ro,
	.multi_io_quirk = ums9117_mmc_multi_io_quirk,
};

static int
ta1618_mmc_map_resource(struct platform_device *pdev,
			const struct ta1618_sdio_resource *definition,
			void __iomem **mapped)
{
	struct resource *resource;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						definition->name);
	if (!resource || resource_type(resource) != IORESOURCE_MEM ||
	    resource->start != definition->address ||
	    resource_size(resource) != definition->size ||
	    !IS_ALIGNED(resource->start, definition->size))
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"resource %s must be at 0x%08x with exact size 0x%x\n",
			definition->name, definition->address,
			definition->size);
	*mapped = devm_ioremap_resource(&pdev->dev, resource);
	return IS_ERR(*mapped) ? PTR_ERR(*mapped) : 0;
}

static int ta1618_mmc_map_sdio_resources(struct platform_device *pdev,
					 struct ta1618_mmc_host *host)
{
	const struct ta1618_sdio_resource *definition;
	struct resource *resource;
	unsigned int index;
	int ret;

	for (index = 0; index < UMS9117_SDIO_REG_COUNT; ++index) {
		definition = ta1618_sdio_controller_resource(index);
		ret = ta1618_mmc_map_resource(pdev, definition,
					      &host->controller_regs[index]);
		if (ret)
			return ret;
	}
	for (index = 0; index < TA1618_SDIO_REG_BOARD_COUNT; ++index) {
		definition = ta1618_sdio_board_resource(index);
		ret = ta1618_mmc_map_resource(pdev, definition,
					      &host->board_regs[index]);
		if (ret)
			return ret;
	}
	definition = ta1618_sdio_adi_resource();
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						definition->name);
	if (!resource || resource->start != definition->address ||
	    resource_size(resource) != definition->size)
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"resource %s must be at 0x%08x with exact size 0x%x\n",
			definition->name, definition->address,
			definition->size);
	definition = ta1618_sdio_analog_resource();
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						definition->name);
	if (!resource || resource->start != definition->address ||
	    resource_size(resource) != definition->size)
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"resource %s must be at 0x%08x with exact size 0x%x\n",
			definition->name, definition->address,
			definition->size);
	if (platform_get_resource(pdev, IORESOURCE_MEM,
				  UMS9117_SDIO_REG_COUNT +
					  TA1618_SDIO_REG_BOARD_COUNT + 2U))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "unexpected extra MMC memory resource\n");
	return 0;
}

static void ta1618_mmc_init_sdio(struct ta1618_mmc_host *host)
{
	host->sdio.context = host;
	host->sdio.read = ta1618_mmc_board_read;
	host->sdio.write = ta1618_mmc_board_write;
	host->sdio.adi_begin = ta1618_mmc_adi_begin;
	host->sdio.adi_read = ta1618_mmc_adi_read;
	host->sdio.adi_write = ta1618_mmc_adi_write;
	host->sdio.adi_end = ta1618_mmc_adi_end;
	host->sdio.sleep_ms = ta1618_mmc_sleep_ms;
	host->sdio.controller.context = host;
	host->sdio.controller.read = ta1618_mmc_controller_read;
	host->sdio.controller.write = ta1618_mmc_controller_write;
	host->sdio.controller.time_us = ta1618_mmc_time_us;
	host->sdio.controller.delay_us = ta1618_mmc_delay_us;
	host->sdio.controller.sleep_us = ta1618_mmc_sleep_us;
	host->sdio.controller.data_barrier = ta1618_mmc_data_barrier;
}

static void ta1618_mmc_restore_card_detect(struct ta1618_mmc_host *host)
{
	int ret;

	WRITE_ONCE(host->card_detect_enabled, false);
	ret = ta1618_sdio_restore_card_detect(&host->sdio, &host->sdio_state);
	if (ret)
		dev_err(host->dev,
			"failed to restore card-detect ownership: %d\n", ret);
}

static int ta1618_mmc_enable_card_detect(struct ta1618_mmc_host *host)
{
	int ret;

	ret = ta1618_sdio_enable_card_detect(&host->sdio, &host->sdio_state);
	if (ret) {
		ta1618_mmc_restore_card_detect(host);
		return ret;
	}
	WRITE_ONCE(host->card_detect_enabled, true);
	return 0;
}

static int ta1618_mmc_probe(struct platform_device *pdev)
{
	struct ta1618_mmc_host *host;
	struct mmc_host *mmc;
	struct irq_data *irq_data;
	irq_hw_number_t hwirq;
	u32 trigger;
	int irq;
	int ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	irq_data = irq_get_irq_data(irq);
	if (!irq_data)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "SPI57 has no IRQ domain data\n");
	hwirq = irqd_to_hwirq(irq_data);
	trigger = irqd_get_trigger_type(irq_data);
	if (hwirq != TA1618_SDIO0_INTID || trigger != IRQ_TYPE_LEVEL_HIGH)
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"IRQ must be SDIO0 SPI%u/INTID%u level-high, got %lu type 0x%x\n",
			TA1618_SDIO0_SPI, TA1618_SDIO0_INTID,
			(unsigned long)hwirq, trigger);

	mmc = mmc_alloc_host(sizeof(*host), &pdev->dev);
	if (!mmc)
		return -ENOMEM;
	host = mmc_priv(mmc);
	host->dev = &pdev->dev;
	host->mmc = mmc;
	host->irq = irq;
	spin_lock_init(&host->lock);
	mutex_init(&host->state_mutex);
	INIT_DELAYED_WORK(&host->timeout_work, ums9117_mmc_timeout_work);
	INIT_WORK(&host->finish_work, ums9117_mmc_finish_work);
	ta1618_mmc_init_sdio(host);
	ret = ta1618_mmc_map_sdio_resources(pdev, host);
	if (ret)
		goto out_free_host;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto out_free_host;
	/*
	 * A descriptor carries a 16-bit length, so a segment must never be
	 * allowed to grow past one page and overflow it.
	 */
	dma_set_max_seg_size(&pdev->dev, PAGE_SIZE);
	host->descriptors =
		dma_alloc_coherent(&pdev->dev, UMS9117_MMC_ADMA2_TABLE_BYTES,
				   &host->descriptors_dma, GFP_KERNEL);
	if (!host->descriptors) {
		ret = -ENOMEM;
		goto out_free_host;
	}
	if (!ta1618_mmc_dma_address_valid(host->descriptors_dma,
					  UMS9117_MMC_ADMA2_TABLE_BYTES)) {
		ret = -ERANGE;
		goto out_free_descriptor;
	}

	ret = ta1618_sdio_snapshot(&host->sdio, &host->sdio_state);
	if (ret)
		goto out_free_descriptor;
	ret = request_irq(irq, ums9117_mmc_irq, 0, dev_name(&pdev->dev), host);
	if (ret)
		goto out_free_descriptor;
	host->irq_requested = true;

	mmc->ops = &ums9117_mmc_ops;
	mmc->f_min = UMS9117_SDIO_IDENT_CLOCK_HZ;
	mmc->f_max = UMS9117_SDIO_HS_CLOCK_HZ;
	mmc->f_init = UMS9117_SDIO_IDENT_CLOCK_HZ;
	mmc->ocr_avail = MMC_VDD_29_30 | MMC_VDD_30_31;
	mmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_SD_HIGHSPEED |
		    MMC_CAP_NEEDS_POLL;
	/*
	 * High speed is claimed but the clock is not raised yet: the core
	 * takes this as the ceiling for a high-speed card, so it keeps asking
	 * for the frequency this board is already qualified at.
	 */
	mmc->max_sd_hs_hz = UMS9117_SDIO_HS_CLOCK_HZ;
	mmc->caps2 = MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC;
	/*
	 * These four numbers are the real limit on request shape: the block
	 * layer derives its own from them, and the core refuses anything that
	 * exceeds them before the driver is ever called. The segment count
	 * must not outgrow the descriptor table.
	 */
	mmc->max_segs = UMS9117_MMC_ADMA2_DESC_COUNT;
	mmc->max_seg_size = PAGE_SIZE;
	mmc->max_req_size = UMS9117_MMC_MAX_REQUEST_BYTES;
	mmc->max_blk_size = 512;
	mmc->max_blk_count = UMS9117_MMC_MAX_REQUEST_BYTES / 512;
	mmc->max_busy_timeout = UMS9117_MMC_REQUEST_TIMEOUT_MS;
	platform_set_drvdata(pdev, host);

	ret = ta1618_mmc_enable_card_detect(host);
	if (ret)
		goto out_restore_card_detect;
	ret = mmc_add_host(mmc);
	if (ret)
		goto out_restore_card_detect;
	ret = device_create_file(&pdev->dev, &dev_attr_audit);
	if (ret)
		goto out_remove_host;
	host->audit_file_created = true;
	dev_notice(
		&pdev->dev,
		"registered 4-bit UMS9117 SDIO0 MMC host on SPI57; identification 1-bit at %u Hz, the operational clock deferred until clean CMD55/ACMD6 and width4, up to %u 32-bit ADMA2 segments and %u bytes per request, multi-block reads and writes with automatic CMD12\n",
		UMS9117_SDIO_IDENT_CLOCK_HZ, UMS9117_MMC_ADMA2_DESC_COUNT,
		UMS9117_MMC_MAX_REQUEST_BYTES);
	return 0;

out_remove_host:
	mmc_remove_host(mmc);
out_restore_card_detect:
	ta1618_mmc_restore_card_detect(host);
	free_irq(irq, host);
	host->irq_requested = false;
out_free_descriptor:
	dma_free_coherent(&pdev->dev, UMS9117_MMC_ADMA2_TABLE_BYTES,
			  host->descriptors, host->descriptors_dma);
out_free_host:
	mmc_free_host(mmc);
	return ret;
}

static bool ta1618_mmc_idle(struct ta1618_mmc_host *host)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&host->lock, flags);
	idle = !host->active_mrq && !host->finish_mrq;
	spin_unlock_irqrestore(&host->lock, flags);
	return idle;
}

static void ta1618_mmc_shutdown(struct platform_device *pdev)
{
	struct ta1618_mmc_host *host = platform_get_drvdata(pdev);
	unsigned long flags;
	ktime_t deadline;
	u32 present = 0;
	int ret;

	spin_lock_irqsave(&host->lock, flags);
	host->stopping = true;
	spin_unlock_irqrestore(&host->lock, flags);

	deadline = ktime_add_ms(ktime_get(), UMS9117_MMC_SHUTDOWN_DEADLINE_MS);
	while (!ta1618_mmc_idle(host) &&
	       ktime_compare(ktime_get(), deadline) < 0)
		msleep(UMS9117_MMC_SHUTDOWN_POLL_MS);
	if (!ta1618_mmc_idle(host))
		dev_err(host->dev,
			"request did not stop before shutdown deadline\n");

	if (host->sdio_state.platform_active)
		ta1618_mmc_controller_write(
			host, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0);
	cancel_delayed_work_sync(&host->timeout_work);
	cancel_work_sync(&host->finish_work);
	if (host->irq_requested)
		synchronize_irq(host->irq);

	mutex_lock(&host->state_mutex);
	if (host->sdio_state.platform_active &&
	    !ums9117_mmc_wait_quiescent(
		    host, UMS9117_MMC_WRITE_QUIESCE_DEADLINE_MS, &present)) {
		dev_err(host->dev,
			"controller did not quiesce for shutdown: present_state=0x%08x\n",
			present);
	} else if (host->sdio_state.platform_active &&
		   host->sdio_state.controller.card_clock_on) {
		ret = ums9117_mmc_set_card_clock(host, false);
		if (ret)
			dev_err(host->dev,
				"failed to stop card clock for shutdown: %d\n",
				ret);
	}
	mutex_unlock(&host->state_mutex);
}

static void ta1618_mmc_remove(struct platform_device *pdev)
{
	struct ta1618_mmc_host *host = platform_get_drvdata(pdev);

	if (host->audit_file_created) {
		device_remove_file(&pdev->dev, &dev_attr_audit);
		host->audit_file_created = false;
	}
	mmc_remove_host(host->mmc);
	ta1618_mmc_restore_card_detect(host);
	WRITE_ONCE(host->stopping, true);
	if (host->sdio_state.platform_active)
		ta1618_mmc_controller_write(
			host, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0);
	cancel_delayed_work_sync(&host->timeout_work);
	cancel_work_sync(&host->finish_work);
	if (host->irq_requested) {
		synchronize_irq(host->irq);
		free_irq(host->irq, host);
		host->irq_requested = false;
	}
	mutex_lock(&host->state_mutex);
	ta1618_mmc_restore_platform(host);
	mutex_unlock(&host->state_mutex);
	dma_free_coherent(host->dev, UMS9117_MMC_ADMA2_TABLE_BYTES,
			  host->descriptors, host->descriptors_dma);
	mmc_free_host(host->mmc);
}

static const struct of_device_id ta1618_mmc_of_match[] = {
	{ .compatible = "fplinux,ta1618-mmc" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_mmc_of_match);

static struct platform_driver ta1618_mmc_driver = {
	.probe = ta1618_mmc_probe,
	.remove = ta1618_mmc_remove,
	.shutdown = ta1618_mmc_shutdown,
	.driver = {
		.name = "ta1618-mmc",
		.of_match_table = ta1618_mmc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ta1618_mmc_driver);

MODULE_DESCRIPTION("UMS9117 SDIO0 microSD host for Nokia TA-1618");
MODULE_LICENSE("GPL");
