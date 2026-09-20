// SPDX-License-Identifier: GPL-2.0-only
#include <asm/io.h>
#include <command.h>
#include <dm.h>
#include <hang.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <mmc.h>
#include <time.h>

#include "stage0-handoff.h"
#include "ums9117-mmc.h"
#include "ums9117-sdio-slot.h"

#ifndef CONFIG_SYS_DCACHE_OFF
#error "UMS9117 MMC has no descriptor/data cache maintenance; CONFIG_SYS_DCACHE_OFF is required"
#endif

#define UMS9117_ADI_RD_CMD 0x28U
#define UMS9117_ADI_RD_DATA 0x2cU
#define UMS9117_ADI_FIFO_STS 0x30U
#define UMS9117_ADI_FIFO_EMPTY 0x00000400U
#define UMS9117_ADI_FIFO_FULL 0x00000800U
#define UMS9117_ADI_TIMEOUT_US 3000U

#define UMS9117_MMC_COMMAND_TIMEOUT_US 1000000U
#define UMS9117_MMC_QUIESCE_TIMEOUT_US 100000U
#define UMS9117_MMC_POLL_US 10U
#define UMS9117_MMC_MAX_READ_BYTES 512U
#define UMS9117_MMC_ACMD6_WIDTH4_ARG 2U
#define UMS9117_MMC_RCA_LOW_MASK 0x0000ffffU
#define UMS9117_MMC_CMD6_CHECK_ARGUMENT 0x00fffff1U
#define UMS9117_MMC_CMD6_SWITCH_ARGUMENT 0x80fffff1U

struct ums9117_mmc_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

struct ums9117_mmc_priv {
	struct ums9117_sdio_slot_io io;
	struct ums9117_sdio_slot_state state;
	struct ums9117_sdio_adma2_desc descriptor __aligned(32);
	struct mmc *mmc;
	bool app_cmd_armed;
	bool width_acmd6_clean;
	u32 app_cmd_argument;
};

static struct ums9117_mmc_priv *ums9117_mmc_instance;

static u32 ums9117_controller_read(void *context, enum ums9117_sdio_reg reg)
{
	const struct ums9117_sdio_resource *resource =
		ums9117_sdio_slot_controller_resource(reg);

	(void)context;
	return readl((void __iomem *)(uintptr_t)resource->address);
}

static void ums9117_controller_write(void *context, enum ums9117_sdio_reg reg,
				     u32 value)
{
	const struct ums9117_sdio_resource *resource =
		ums9117_sdio_slot_controller_resource(reg);

	(void)context;
	writel(value, (void __iomem *)(uintptr_t)resource->address);
}

static u32 ums9117_slot_read(void *context, enum ums9117_sdio_slot_reg reg)
{
	const struct ums9117_sdio_resource *resource =
		ums9117_sdio_slot_resource(reg);

	(void)context;
	return readl((void __iomem *)(uintptr_t)resource->address);
}

static void ums9117_slot_write(void *context, enum ums9117_sdio_slot_reg reg,
			       u32 value)
{
	const struct ums9117_sdio_resource *resource =
		ums9117_sdio_slot_resource(reg);

	(void)context;
	writel(value, (void __iomem *)(uintptr_t)resource->address);
}

static u64 ums9117_time_us(void *context)
{
	(void)context;
	return timer_get_us();
}

static void ums9117_delay_us(void *context, u32 usec)
{
	(void)context;
	udelay(usec);
}

static void ums9117_sleep_us(void *context, u32 min, u32 max)
{
	(void)context;
	(void)max;
	udelay(min);
}

static void ums9117_sleep_ms(void *context, u32 msec)
{
	(void)context;
	mdelay(msec);
}

static void ums9117_data_barrier(void *context)
{
	(void)context;
	/* This orders uncached accesses; it does not provide DMA cache coherence. */
	mb();
}

static int ums9117_adi_begin(void *context)
{
	(void)context;
	return 0;
}

static int ums9117_adi_end(void *context)
{
	(void)context;
	return 0;
}

static int ums9117_adi_read(void *context,
			    enum ums9117_sdio_slot_analog_reg reg, u16 *value)
{
	u32 base = ums9117_sdio_slot_adi_resource()->address;
	u64 started = timer_get_us();
	s32 data;

	(void)context;
	/* Stage0 leaves ADI in the vendor full-physical-address mode. */
	writel(ums9117_sdio_slot_analog_address(reg),
	       (void __iomem *)(uintptr_t)(base + UMS9117_ADI_RD_CMD));
	for (;;) {
		data = (s32)readl((
			void __iomem *)(uintptr_t)(base + UMS9117_ADI_RD_DATA));
		if (data >= 0) {
			*value = (u16)data;
			return 0;
		}
		if (timer_get_us() - started >= UMS9117_ADI_TIMEOUT_US)
			return -ETIMEDOUT;
		udelay(1);
	}
}

static int ums9117_adi_write(void *context,
			     enum ums9117_sdio_slot_analog_reg reg, u16 value)
{
	u32 base = ums9117_sdio_slot_adi_resource()->address;
	u32 address = ums9117_sdio_slot_analog_address(reg);
	u64 started = timer_get_us();
	u16 readback;
	int ret;

	while (readl((void __iomem *)(uintptr_t)(base + UMS9117_ADI_FIFO_STS)) &
	       UMS9117_ADI_FIFO_FULL) {
		if (timer_get_us() - started >= UMS9117_ADI_TIMEOUT_US)
			return -ETIMEDOUT;
		udelay(1);
	}
	writel(value, (void __iomem *)(uintptr_t)address);
	mb();
	started = timer_get_us();
	while (!(readl((void __iomem *)(uintptr_t)(base +
						   UMS9117_ADI_FIFO_STS)) &
		 UMS9117_ADI_FIFO_EMPTY)) {
		if (timer_get_us() - started >= UMS9117_ADI_TIMEOUT_US)
			return -ETIMEDOUT;
		udelay(1);
	}
	ret = ums9117_adi_read(context, reg, &readback);
	if (ret)
		return ret;
	return readback == value ? 0 : -EIO;
}

static const struct ums9117_sdio_slot_io sdio_io = {
	.controller = {
		.read = ums9117_controller_read,
		.write = ums9117_controller_write,
		.time_us = ums9117_time_us,
		.delay_us = ums9117_delay_us,
		.sleep_us = ums9117_sleep_us,
		.data_barrier = ums9117_data_barrier,
	},
	.read = ums9117_slot_read,
	.write = ums9117_slot_write,
	.adi_begin = ums9117_adi_begin,
	.adi_read = ums9117_adi_read,
	.adi_write = ums9117_adi_write,
	.adi_end = ums9117_adi_end,
	.sleep_ms = ums9117_sleep_ms,
};

static void ums9117_mmc_reset_protocol_state(struct ums9117_mmc_priv *priv)
{
	priv->app_cmd_armed = false;
	priv->width_acmd6_clean = false;
	priv->app_cmd_argument = 0;
}

static void ums9117_mmc_cleanup_failed(int error)
{
	const struct fplinux_stage0_ops *ops = ums9117_stage0_ops();
	u32 detail = error < 0 ? (u32)-error : (u32)error;

	if (ops)
		ops->fail(FPLINUX_STAGE0_FAILURE_STORAGE_CLEANUP, detail);
	hang();
}

static void ums9117_mmc_cleanup(struct ums9117_mmc_priv *priv)
{
	int ret = ums9117_sdio_slot_cleanup(&priv->io, &priv->state,
					    UMS9117_MMC_QUIESCE_TIMEOUT_US);

	ums9117_mmc_reset_protocol_state(priv);
	if (ret)
		ums9117_mmc_cleanup_failed(ret);
}

static int ums9117_mmc_response_flags(const struct mmc_cmd *cmd, u16 *flags)
{
	enum ums9117_sdio_response_type type;

	if (cmd->resp_type == MMC_RSP_NONE)
		type = UMS9117_SDIO_RESPONSE_NONE;
	else if (cmd->resp_type == MMC_RSP_R2)
		type = UMS9117_SDIO_RESPONSE_LONG;
	else if (cmd->resp_type == MMC_RSP_R1b)
		type = UMS9117_SDIO_RESPONSE_SHORT_BUSY;
	else if (cmd->resp_type == MMC_RSP_R3)
		type = UMS9117_SDIO_RESPONSE_OCR;
	else if (cmd->resp_type == MMC_RSP_R1 || cmd->resp_type == MMC_RSP_R6 ||
		 cmd->resp_type == MMC_RSP_R7)
		type = UMS9117_SDIO_RESPONSE_SHORT;
	else
		return -EOPNOTSUPP;
	return ums9117_sdio_response_flags(type, flags);
}

static int ums9117_mmc_response_error(const struct mmc_cmd *cmd)
{
	/* U-Boot encodes R1, R6 and R7 with the same response flag bits. */
	if (cmd->cmdidx == SD_CMD_SEND_RELATIVE_ADDR ||
	    cmd->cmdidx == SD_CMD_SEND_IF_COND)
		return 0;
	if (cmd->resp_type != MMC_RSP_R1 && cmd->resp_type != MMC_RSP_R1b)
		return 0;
	return ums9117_sdio_r1_error(cmd->response[0]);
}

static int ums9117_mmc_qualify(struct ums9117_mmc_priv *priv,
			       const struct mmc_cmd *cmd,
			       const struct mmc_data *data, bool *width_acmd6)
{
	const struct ums9117_sdio_state *controller = &priv->state.controller;
	bool app_context = priv->app_cmd_armed;
	u32 app_argument = priv->app_cmd_argument;

	*width_acmd6 = false;
	if (cmd->cmdidx != MMC_CMD_APP_CMD) {
		priv->app_cmd_armed = false;
		priv->app_cmd_argument = 0;
	}
	if (cmd->cmdidx > 63U)
		return -EOPNOTSUPP;
	if (data) {
		if (data->flags != MMC_DATA_READ || data->blocks != 1U ||
		    !data->blocksize ||
		    data->blocksize > UMS9117_MMC_MAX_READ_BYTES || !data->dest)
			return -EOPNOTSUPP;
		if (cmd->cmdidx == SD_CMD_APP_SEND_SCR)
			return app_context && app_argument &&
					       !(app_argument &
						 UMS9117_MMC_RCA_LOW_MASK) &&
					       data->blocksize == 8U ?
				       0 :
				       -EPROTO;
		if (cmd->cmdidx == SD_CMD_SWITCH_FUNC) {
			if (app_context || data->blocksize != 64U ||
			    (cmd->cmdarg != UMS9117_MMC_CMD6_CHECK_ARGUMENT &&
			     cmd->cmdarg != UMS9117_MMC_CMD6_SWITCH_ARGUMENT))
				return -EOPNOTSUPP;
			if (cmd->cmdarg == UMS9117_MMC_CMD6_CHECK_ARGUMENT)
				return !controller->physical_width4 &&
						       controller->actual_clock_hz ==
							       UMS9117_SDIO_IDENT_CLOCK_HZ ?
					       0 :
					       -EPROTO;
			return controller->physical_width4 &&
					       priv->width_acmd6_clean &&
					       controller->actual_clock_hz ==
						       UMS9117_SDIO_LEGACY_CLOCK_HZ ?
				       0 :
				       -EPROTO;
		}
		if (cmd->cmdidx == MMC_CMD_READ_SINGLE_BLOCK)
			return !app_context && data->blocksize == 512U &&
					       controller->physical_width4 &&
					       (controller->actual_clock_hz ==
							UMS9117_SDIO_LEGACY_CLOCK_HZ ||
						controller->actual_clock_hz ==
							UMS9117_SDIO_HS_CLOCK_HZ) ?
				       0 :
				       -EPROTO;
		return -EOPNOTSUPP;
	}

	if (cmd->cmdidx == SD_CMD_APP_SET_BUS_WIDTH) {
		if (!app_context || !app_argument ||
		    (app_argument & UMS9117_MMC_RCA_LOW_MASK) ||
		    cmd->cmdarg != UMS9117_MMC_ACMD6_WIDTH4_ARG ||
		    controller->physical_width4)
			return -EPROTO;
		*width_acmd6 = true;
		return 0;
	}
	if (cmd->cmdidx == SD_CMD_APP_SEND_OP_COND)
		return app_context && !app_argument ? 0 : -EPROTO;
	if (app_context)
		return -EPROTO;

	switch (cmd->cmdidx) {
	case MMC_CMD_GO_IDLE_STATE:
	case MMC_CMD_ALL_SEND_CID:
	case MMC_CMD_SET_RELATIVE_ADDR:
	case MMC_CMD_SET_DSR:
	case MMC_CMD_SELECT_CARD:
	case SD_CMD_SEND_IF_COND:
	case MMC_CMD_SEND_CSD:
	case MMC_CMD_SEND_CID:
	case MMC_CMD_STOP_TRANSMISSION:
	case MMC_CMD_SEND_STATUS:
	case MMC_CMD_SET_BLOCKLEN:
	case MMC_CMD_APP_CMD:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int ums9117_mmc_prepare_read(struct ums9117_mmc_priv *priv,
				    const struct mmc_data *data)
{
	uintptr_t address = (uintptr_t)data->dest;
	uintptr_t descriptor = (uintptr_t)&priv->descriptor;
	u32 length = data->blocksize;

	if (!address || address > UINT32_MAX ||
	    length - 1U > UINT32_MAX - address || !IS_ALIGNED(address, 4U) ||
	    descriptor > UINT32_MAX || !IS_ALIGNED(descriptor, 4U))
		return -ERANGE;
	priv->descriptor.attr = cpu_to_le16(UMS9117_SDIO_ADMA2_TRANSFER_END);
	priv->descriptor.length = cpu_to_le16((u16)length);
	priv->descriptor.address = cpu_to_le32((u32)address);
	/* The cache-off build contract makes the following barrier sufficient. */
	mb();
	return 0;
}

static int ums9117_mmc_issue(struct ums9117_mmc_priv *priv, struct mmc_cmd *cmd,
			     struct mmc_data *data, u16 response_flags)
{
	struct ums9117_sdio_completion completion;
	struct ums9117_sdio_data_setup setup;
	u64 started;
	u32 status;
	u32 terminal;
	u32 required;
	u16 command;
	u16 transfer = 0;
	int ret;

	ret = ums9117_sdio_slot_validate_active(&priv->io, &priv->state);
	if (ret)
		return ret;
	ret = ums9117_sdio_wait_inhibit(&priv->io.controller, true);
	if (ret)
		return ret;
	if (data) {
		ret = ums9117_mmc_prepare_read(priv, data);
		if (ret)
			return ret;
		setup.blocks = data->blocks;
		setup.block_size = data->blocksize;
		setup.adma_address = (u32)(uintptr_t)&priv->descriptor;
		ret = ums9117_sdio_prepare_request(&priv->io.controller,
						   &setup);
		response_flags |= UMS9117_SDIO_CMD_DATA;
		transfer = UMS9117_SDIO_TRANSFER_READ_ADMA2;
		terminal = UMS9117_SDIO_INT_TRANSFER;
		required = UMS9117_SDIO_INT_RESPONSE |
			   UMS9117_SDIO_INT_TRANSFER;
	} else {
		ret = ums9117_sdio_prepare_request(&priv->io.controller, NULL);
		response_flags |= UMS9117_SDIO_SUB_CMD;
		terminal = cmd->resp_type & MMC_RSP_BUSY ?
				   UMS9117_SDIO_INT_TRANSFER :
				   UMS9117_SDIO_INT_RESPONSE;
		required = cmd->resp_type & MMC_RSP_BUSY ?
				   UMS9117_SDIO_INT_RESPONSE |
					   UMS9117_SDIO_INT_TRANSFER :
				   UMS9117_SDIO_INT_RESPONSE;
	}
	if (ret)
		return ret;
	command = (u16)((cmd->cmdidx << 8) | response_flags);
	ums9117_sdio_issue_request(&priv->io.controller, cmd->cmdarg, command,
				   transfer, 0);
	started = timer_get_us();
	for (;;) {
		status = ums9117_controller_read(
			NULL, UMS9117_SDIO_REG_INTERRUPT_STATUS);
		if (ums9117_sdio_status_terminal(status, terminal)) {
			ret = ums9117_sdio_status_error(status);
			if (ret && ret != -ETIMEDOUT)
				ret = -EIO;
			break;
		}
		if (timer_get_us() - started >=
		    UMS9117_MMC_COMMAND_TIMEOUT_US) {
			ret = -ETIMEDOUT;
			break;
		}
		udelay(UMS9117_MMC_POLL_US);
	}
	ums9117_sdio_capture_completion(&priv->io.controller, status,
					!!(cmd->resp_type & MMC_RSP_136), false,
					UMS9117_SDIO_RESPONSE_BEFORE_ACK,
					&completion);
	if (completion.status_readback & completion.owned_status) {
		ret = -EIO;
	} else if (!ret) {
		ret = ums9117_sdio_validate_completion(&completion, required);
		if (ret && ret != -ETIMEDOUT)
			ret = -EIO;
	}
	if (!ret && (data || (cmd->resp_type & MMC_RSP_BUSY)))
		ret = ums9117_sdio_wait_quiescent(
			&priv->io.controller, UMS9117_MMC_QUIESCE_TIMEOUT_US,
			NULL);
	cmd->response[0] = completion.response[0];
	cmd->response[1] = completion.response[1];
	cmd->response[2] = completion.response[2];
	cmd->response[3] = completion.response[3];
	return ret;
}

static int ums9117_mmc_send_cmd(struct udevice *dev, struct mmc_cmd *cmd,
				struct mmc_data *data)
{
	struct ums9117_mmc_priv *priv = dev_get_priv(dev);
	bool width_acmd6;
	u16 response_flags;
	int ret;

	ret = ums9117_mmc_qualify(priv, cmd, data, &width_acmd6);
	if (ret)
		return ret;
	ret = ums9117_mmc_response_flags(cmd, &response_flags);
	if (ret)
		return ret;
	ret = ums9117_mmc_issue(priv, cmd, data, response_flags);
	if (!ret)
		ret = ums9117_mmc_response_error(cmd);
	if (ret) {
		printf("ums9117-mmc: command %u failed: %d\n", cmd->cmdidx,
		       ret);
		ums9117_mmc_cleanup(priv);
		return ret;
	}
	if (cmd->cmdidx == MMC_CMD_APP_CMD) {
		priv->app_cmd_armed = !!(cmd->response[0] & R1_APP_CMD);
		priv->app_cmd_argument = priv->app_cmd_armed ? cmd->cmdarg : 0;
	}
	if (width_acmd6)
		priv->width_acmd6_clean = true;
	return 0;
}

static int ums9117_mmc_set_clock(struct ums9117_mmc_priv *priv,
				 enum ums9117_sdio_clock_profile profile)
{
	bool first_width = !priv->state.controller.physical_width4;

	if (!priv->state.platform_active || !priv->state.rails_on ||
	    (first_width && (!priv->width_acmd6_clean ||
			     profile == UMS9117_SDIO_CLOCK_HIGH_SPEED)) ||
	    (!first_width && profile == UMS9117_SDIO_CLOCK_HIGH_SPEED &&
	     !priv->width_acmd6_clean))
		return -EPROTO;
	return ums9117_sdio_slot_set_operational_clock(&priv->io, &priv->state,
						       profile, NULL);
}

static int ums9117_mmc_set_ios(struct udevice *dev)
{
	struct ums9117_mmc_plat *plat = dev_get_plat(dev);
	struct ums9117_mmc_priv *priv = dev_get_priv(dev);
	struct mmc *mmc = &plat->mmc;
	int ret;

	if (mmc->clk_disable) {
		if (priv->state.platform_active ||
		    priv->state.card_detect_owned)
			ums9117_mmc_cleanup(priv);
		return 0;
	}
	if (!priv->state.platform_active)
		return -EIO;
	if (mmc->bus_width == 1U) {
		if (priv->state.controller.physical_width4)
			return -EPROTO;
		return ums9117_sdio_slot_validate_active(&priv->io,
							 &priv->state);
	}
	if (mmc->bus_width != 4U)
		return -EOPNOTSUPP;
	ret = ums9117_mmc_set_clock(priv, UMS9117_SDIO_CLOCK_LEGACY);
	if (ret)
		goto fail_closed;
	if (mmc->selected_mode == SD_HS &&
	    mmc->clock >= UMS9117_SDIO_HS_CLOCK_HZ) {
		ret = ums9117_mmc_set_clock(priv,
					    UMS9117_SDIO_CLOCK_HIGH_SPEED);
		if (ret)
			goto fail_closed;
	}
	return 0;

fail_closed:
	ums9117_mmc_cleanup(priv);
	return ret;
}

static int ums9117_mmc_get_cd(struct udevice *dev)
{
	struct ums9117_mmc_priv *priv = dev_get_priv(dev);
	int present;
	int ret;

	if (!priv->state.card_detect_owned) {
		ret = ums9117_sdio_slot_enable_card_detect(&priv->io,
							   &priv->state);
		if (ret) {
			if (priv->state.card_detect_owned ||
			    priv->state.platform_active)
				ums9117_mmc_cleanup(priv);
			return 0;
		}
	}
	present = ums9117_sdio_slot_card_present(&priv->io, &priv->state);
	if ((present <= 0 && priv->state.platform_active) ||
	    (present < 0 && priv->state.card_detect_owned))
		ums9117_mmc_cleanup(priv);
	return present > 0;
}

static int ums9117_mmc_get_wp(struct udevice *dev)
{
	(void)dev;
	return 1;
}

static int ums9117_mmc_host_power_cycle(struct udevice *dev)
{
	struct ums9117_mmc_priv *priv = dev_get_priv(dev);

	if (priv->state.platform_active || priv->state.card_detect_owned)
		ums9117_mmc_cleanup(priv);
	return 0;
}

static int ums9117_mmc_reinit(struct udevice *dev)
{
	struct ums9117_mmc_priv *priv = dev_get_priv(dev);
	int present;
	int ret;

	if (priv->state.platform_active || priv->state.card_detect_owned)
		ums9117_mmc_cleanup(priv);
	if (!priv->state.snapshots_valid) {
		ret = ums9117_sdio_slot_snapshot(&priv->io, &priv->state);
		if (ret)
			return ret;
	}
	ret = ums9117_sdio_slot_enable_card_detect(&priv->io, &priv->state);
	if (ret) {
		if (priv->state.card_detect_owned ||
		    priv->state.platform_active)
			ums9117_mmc_cleanup(priv);
		return ret;
	}
	present = ums9117_sdio_slot_card_present(&priv->io, &priv->state);
	if (present < 0) {
		ums9117_mmc_cleanup(priv);
		return present;
	}
	if (!present)
		return -ENOMEDIUM;
	ret = ums9117_sdio_slot_activate(&priv->io, &priv->state);
	if (!ret)
		ret = ums9117_sdio_slot_enable_ident_clock(&priv->io,
							   &priv->state, NULL);
	if (ret) {
		ums9117_mmc_cleanup(priv);
		return ret;
	}
	ums9117_mmc_reset_protocol_state(priv);
	if (priv->mmc)
		priv->mmc->clk_disable = false;
	return 0;
}

static int ums9117_mmc_get_b_max(struct udevice *dev, void *dst,
				 lbaint_t blocks)
{
	(void)dev;
	(void)dst;
	(void)blocks;
	return 1;
}

static const struct dm_mmc_ops ums9117_mmc_ops = {
	.send_cmd = ums9117_mmc_send_cmd,
	.set_ios = ums9117_mmc_set_ios,
	.get_cd = ums9117_mmc_get_cd,
	.get_wp = ums9117_mmc_get_wp,
	.host_power_cycle = ums9117_mmc_host_power_cycle,
	.reinit = ums9117_mmc_reinit,
	.get_b_max = ums9117_mmc_get_b_max,
};

static int ums9117_mmc_bind(struct udevice *dev)
{
	struct ums9117_mmc_plat *plat = dev_get_plat(dev);
	struct mmc_config *cfg = &plat->cfg;

	cfg->name = dev->name;
	cfg->host_caps = MMC_MODE_1BIT | MMC_MODE_4BIT | MMC_MODE_HS;
	cfg->voltages = MMC_VDD_29_30 | MMC_VDD_30_31;
	cfg->f_min = UMS9117_SDIO_IDENT_CLOCK_HZ;
	cfg->f_max = UMS9117_SDIO_HS_CLOCK_HZ;
	cfg->b_max = 1;
	return mmc_bind(dev, &plat->mmc, cfg);
}

static int ums9117_mmc_probe(struct udevice *dev)
{
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct ums9117_mmc_plat *plat = dev_get_plat(dev);
	struct ums9117_mmc_priv *priv = dev_get_priv(dev);
	int ret;

	priv->io = sdio_io;
	priv->io.context = priv;
	ret = ums9117_sdio_slot_snapshot(&priv->io, &priv->state);
	if (ret)
		return ret;
	ret = ums9117_sdio_slot_enable_card_detect(&priv->io, &priv->state);
	if (ret) {
		ums9117_mmc_cleanup(priv);
		return ret;
	}
	plat->mmc.priv = priv;
	upriv->mmc = &plat->mmc;
	priv->mmc = &plat->mmc;
	ums9117_mmc_instance = priv;
	return 0;
}

static int ums9117_mmc_remove(struct udevice *dev)
{
	struct ums9117_mmc_priv *priv = dev_get_priv(dev);

	if (priv->state.snapshots_valid)
		ums9117_mmc_cleanup(priv);
	if (ums9117_mmc_instance == priv)
		ums9117_mmc_instance = NULL;
	return 0;
}

void ums9117_mmc_release(void)
{
	if (ums9117_mmc_instance &&
	    (ums9117_mmc_instance->state.platform_active ||
	     ums9117_mmc_instance->state.card_detect_owned))
		ums9117_mmc_cleanup(ums9117_mmc_instance);
	if (ums9117_mmc_instance && ums9117_mmc_instance->mmc) {
		ums9117_mmc_instance->mmc->has_init = 0;
		ums9117_mmc_instance->mmc->init_in_progress = 0;
		ums9117_mmc_instance->mmc->op_cond_pending = 0;
	}
}

static int do_ums9117_sdrelease(struct cmd_tbl *cmdtp, int flag, int argc,
				char *const argv[])
{
	(void)cmdtp;
	(void)flag;
	(void)argc;
	(void)argv;

	ums9117_mmc_release();
	puts("sdrelease: controller and slot state restored\n");
	return CMD_RET_SUCCESS;
}

static const struct udevice_id ums9117_mmc_ids[] = {
	{ .compatible = "fplinux,ums9117-mmc" },
	{}
};

U_BOOT_DRIVER(ums9117_mmc) = {
	.name = "ums9117_mmc",
	.id = UCLASS_MMC,
	.of_match = ums9117_mmc_ids,
	.bind = ums9117_mmc_bind,
	.probe = ums9117_mmc_probe,
	.remove = ums9117_mmc_remove,
	.ops = &ums9117_mmc_ops,
	.plat_auto = sizeof(struct ums9117_mmc_plat),
	.priv_auto = sizeof(struct ums9117_mmc_priv),
};

U_BOOT_CMD(sdrelease, 1, 0, do_ums9117_sdrelease,
	   "restore the UMS9117 MMC controller and slot baseline", "");
