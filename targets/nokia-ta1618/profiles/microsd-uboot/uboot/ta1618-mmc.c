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
#include "ta1618-mmc.h"
#include "ta1618-sdio-board.h"

#ifndef CONFIG_SYS_DCACHE_OFF
#error "TA-1618 MMC has no descriptor/data cache maintenance; CONFIG_SYS_DCACHE_OFF is required"
#endif

#define TA1618_ADI_RD_CMD 0x28U
#define TA1618_ADI_RD_DATA 0x2cU
#define TA1618_ADI_FIFO_STS 0x30U
#define TA1618_ADI_FIFO_EMPTY 0x00000400U
#define TA1618_ADI_FIFO_FULL 0x00000800U
#define TA1618_ADI_TIMEOUT_US 3000U

#define TA1618_MMC_COMMAND_TIMEOUT_US 1000000U
#define TA1618_MMC_QUIESCE_TIMEOUT_US 100000U
#define TA1618_MMC_POLL_US 10U
#define TA1618_MMC_MAX_READ_BYTES 512U
#define TA1618_MMC_ACMD6_WIDTH4_ARG 2U
#define TA1618_MMC_RCA_LOW_MASK 0x0000ffffU
#define TA1618_MMC_CMD6_CHECK_ARGUMENT 0x00fffff1U
#define TA1618_MMC_CMD6_SWITCH_ARGUMENT 0x80fffff1U

struct ta1618_mmc_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

struct ta1618_mmc_priv {
	struct ta1618_sdio_state state;
	struct ums9117_sdio_adma2_desc descriptor __aligned(32);
	struct mmc *mmc;
	bool app_cmd_armed;
	bool width_acmd6_clean;
	u32 app_cmd_argument;
};

static struct ta1618_mmc_priv *ta1618_mmc_instance;

static u32 ta1618_controller_read(void *context, enum ums9117_sdio_reg reg)
{
	const struct ta1618_sdio_resource *resource =
		ta1618_sdio_controller_resource(reg);

	(void)context;
	return readl((void __iomem *)(uintptr_t)resource->address);
}

static void ta1618_controller_write(void *context, enum ums9117_sdio_reg reg,
				    u32 value)
{
	const struct ta1618_sdio_resource *resource =
		ta1618_sdio_controller_resource(reg);

	(void)context;
	writel(value, (void __iomem *)(uintptr_t)resource->address);
}

static u32 ta1618_board_read(void *context, enum ta1618_sdio_board_reg reg)
{
	const struct ta1618_sdio_resource *resource =
		ta1618_sdio_board_resource(reg);

	(void)context;
	return readl((void __iomem *)(uintptr_t)resource->address);
}

static void ta1618_board_write(void *context, enum ta1618_sdio_board_reg reg,
			       u32 value)
{
	const struct ta1618_sdio_resource *resource =
		ta1618_sdio_board_resource(reg);

	(void)context;
	writel(value, (void __iomem *)(uintptr_t)resource->address);
}

static u64 ta1618_time_us(void *context)
{
	(void)context;
	return timer_get_us();
}

static void ta1618_delay_us(void *context, u32 usec)
{
	(void)context;
	udelay(usec);
}

static void ta1618_sleep_us(void *context, u32 min, u32 max)
{
	(void)context;
	(void)max;
	udelay(min);
}

static void ta1618_sleep_ms(void *context, u32 msec)
{
	(void)context;
	mdelay(msec);
}

static void ta1618_data_barrier(void *context)
{
	(void)context;
	/* This orders uncached accesses; it does not provide DMA cache coherence. */
	mb();
}

static int ta1618_adi_begin(void *context)
{
	(void)context;
	return 0;
}

static int ta1618_adi_end(void *context)
{
	(void)context;
	return 0;
}

static int ta1618_adi_read(void *context, enum ta1618_sdio_analog_reg reg,
			   u16 *value)
{
	u32 base = ta1618_sdio_adi_resource()->address;
	u64 started = timer_get_us();
	s32 data;

	(void)context;
	/* Stage0 leaves ADI in the vendor full-physical-address mode. */
	writel(ta1618_sdio_analog_address(reg),
	       (void __iomem *)(uintptr_t)(base + TA1618_ADI_RD_CMD));
	for (;;) {
		data = (s32)readl(
			(void __iomem *)(uintptr_t)(base + TA1618_ADI_RD_DATA));
		if (data >= 0) {
			*value = (u16)data;
			return 0;
		}
		if (timer_get_us() - started >= TA1618_ADI_TIMEOUT_US)
			return -ETIMEDOUT;
		udelay(1);
	}
}

static int ta1618_adi_write(void *context, enum ta1618_sdio_analog_reg reg,
			    u16 value)
{
	u32 base = ta1618_sdio_adi_resource()->address;
	u32 address = ta1618_sdio_analog_address(reg);
	u64 started = timer_get_us();
	u16 readback;
	int ret;

	while (readl((void __iomem *)(uintptr_t)(base + TA1618_ADI_FIFO_STS)) &
	       TA1618_ADI_FIFO_FULL) {
		if (timer_get_us() - started >= TA1618_ADI_TIMEOUT_US)
			return -ETIMEDOUT;
		udelay(1);
	}
	writel(value, (void __iomem *)(uintptr_t)address);
	mb();
	started = timer_get_us();
	while (!(
		readl((void __iomem *)(uintptr_t)(base + TA1618_ADI_FIFO_STS)) &
		TA1618_ADI_FIFO_EMPTY)) {
		if (timer_get_us() - started >= TA1618_ADI_TIMEOUT_US)
			return -ETIMEDOUT;
		udelay(1);
	}
	ret = ta1618_adi_read(context, reg, &readback);
	if (ret)
		return ret;
	return readback == value ? 0 : -EIO;
}

static const struct ta1618_sdio_io ta1618_sdio = {
	.controller = {
		.read = ta1618_controller_read,
		.write = ta1618_controller_write,
		.time_us = ta1618_time_us,
		.delay_us = ta1618_delay_us,
		.sleep_us = ta1618_sleep_us,
		.data_barrier = ta1618_data_barrier,
	},
	.read = ta1618_board_read,
	.write = ta1618_board_write,
	.adi_begin = ta1618_adi_begin,
	.adi_read = ta1618_adi_read,
	.adi_write = ta1618_adi_write,
	.adi_end = ta1618_adi_end,
	.sleep_ms = ta1618_sleep_ms,
};

static void ta1618_mmc_reset_protocol_state(struct ta1618_mmc_priv *priv)
{
	priv->app_cmd_armed = false;
	priv->width_acmd6_clean = false;
	priv->app_cmd_argument = 0;
}

static void ta1618_mmc_cleanup_failed(int error)
{
	const struct fplinux_stage0_ops *ops = fplinux_stage0_ops();
	u32 detail = error < 0 ? (u32)-error : (u32)error;

	if (ops)
		ops->fail(FPLINUX_STAGE0_FAILURE_STORAGE_CLEANUP, detail);
	hang();
}

static void ta1618_mmc_cleanup(struct ta1618_mmc_priv *priv)
{
	int ret = ta1618_sdio_cleanup(&ta1618_sdio, &priv->state,
				      TA1618_MMC_QUIESCE_TIMEOUT_US);

	ta1618_mmc_reset_protocol_state(priv);
	if (ret)
		ta1618_mmc_cleanup_failed(ret);
}

static int ta1618_mmc_response_flags(const struct mmc_cmd *cmd, u16 *flags)
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

static int ta1618_mmc_response_error(const struct mmc_cmd *cmd)
{
	/* U-Boot encodes R1, R6 and R7 with the same response flag bits. */
	if (cmd->cmdidx == SD_CMD_SEND_RELATIVE_ADDR ||
	    cmd->cmdidx == SD_CMD_SEND_IF_COND)
		return 0;
	if (cmd->resp_type != MMC_RSP_R1 && cmd->resp_type != MMC_RSP_R1b)
		return 0;
	return ums9117_sdio_r1_error(cmd->response[0]);
}

static int ta1618_mmc_qualify(struct ta1618_mmc_priv *priv,
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
		    data->blocksize > TA1618_MMC_MAX_READ_BYTES || !data->dest)
			return -EOPNOTSUPP;
		if (cmd->cmdidx == SD_CMD_APP_SEND_SCR)
			return app_context && app_argument &&
					       !(app_argument &
						 TA1618_MMC_RCA_LOW_MASK) &&
					       data->blocksize == 8U ?
				       0 :
				       -EPROTO;
		if (cmd->cmdidx == SD_CMD_SWITCH_FUNC) {
			if (app_context || data->blocksize != 64U ||
			    (cmd->cmdarg != TA1618_MMC_CMD6_CHECK_ARGUMENT &&
			     cmd->cmdarg != TA1618_MMC_CMD6_SWITCH_ARGUMENT))
				return -EOPNOTSUPP;
			if (cmd->cmdarg == TA1618_MMC_CMD6_CHECK_ARGUMENT)
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
		    (app_argument & TA1618_MMC_RCA_LOW_MASK) ||
		    cmd->cmdarg != TA1618_MMC_ACMD6_WIDTH4_ARG ||
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

static int ta1618_mmc_prepare_read(struct ta1618_mmc_priv *priv,
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

static int ta1618_mmc_issue(struct ta1618_mmc_priv *priv, struct mmc_cmd *cmd,
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

	ret = ta1618_sdio_validate_active(&ta1618_sdio, &priv->state);
	if (ret)
		return ret;
	ret = ums9117_sdio_wait_inhibit(&ta1618_sdio.controller, true);
	if (ret)
		return ret;
	if (data) {
		ret = ta1618_mmc_prepare_read(priv, data);
		if (ret)
			return ret;
		setup.blocks = data->blocks;
		setup.block_size = data->blocksize;
		setup.adma_address = (u32)(uintptr_t)&priv->descriptor;
		ret = ums9117_sdio_prepare_request(&ta1618_sdio.controller,
						   &setup);
		response_flags |= UMS9117_SDIO_CMD_DATA;
		transfer = UMS9117_SDIO_TRANSFER_READ_ADMA2;
		terminal = UMS9117_SDIO_INT_TRANSFER;
		required = UMS9117_SDIO_INT_RESPONSE |
			   UMS9117_SDIO_INT_TRANSFER;
	} else {
		ret = ums9117_sdio_prepare_request(&ta1618_sdio.controller,
						   NULL);
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
	ums9117_sdio_issue_request(&ta1618_sdio.controller, cmd->cmdarg,
				   command, transfer, 0);
	started = timer_get_us();
	for (;;) {
		status = ta1618_controller_read(
			NULL, UMS9117_SDIO_REG_INTERRUPT_STATUS);
		if (ums9117_sdio_status_terminal(status, terminal)) {
			ret = ums9117_sdio_status_error(status);
			if (ret && ret != -ETIMEDOUT)
				ret = -EIO;
			break;
		}
		if (timer_get_us() - started >= TA1618_MMC_COMMAND_TIMEOUT_US) {
			ret = -ETIMEDOUT;
			break;
		}
		udelay(TA1618_MMC_POLL_US);
	}
	ums9117_sdio_capture_completion(&ta1618_sdio.controller, status,
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
		ret = ums9117_sdio_wait_quiescent(&ta1618_sdio.controller,
						  TA1618_MMC_QUIESCE_TIMEOUT_US,
						  NULL);
	cmd->response[0] = completion.response[0];
	cmd->response[1] = completion.response[1];
	cmd->response[2] = completion.response[2];
	cmd->response[3] = completion.response[3];
	return ret;
}

static int ta1618_mmc_send_cmd(struct udevice *dev, struct mmc_cmd *cmd,
			       struct mmc_data *data)
{
	struct ta1618_mmc_priv *priv = dev_get_priv(dev);
	bool width_acmd6;
	u16 response_flags;
	int ret;

	ret = ta1618_mmc_qualify(priv, cmd, data, &width_acmd6);
	if (ret)
		return ret;
	ret = ta1618_mmc_response_flags(cmd, &response_flags);
	if (ret)
		return ret;
	ret = ta1618_mmc_issue(priv, cmd, data, response_flags);
	if (!ret)
		ret = ta1618_mmc_response_error(cmd);
	if (ret) {
		printf("ta1618-mmc: command %u failed: %d\n", cmd->cmdidx, ret);
		ta1618_mmc_cleanup(priv);
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

static int ta1618_mmc_set_clock(struct ta1618_mmc_priv *priv,
				enum ums9117_sdio_clock_profile profile)
{
	bool first_width = !priv->state.controller.physical_width4;

	if (!priv->state.platform_active || !priv->state.rails_on ||
	    (first_width && (!priv->width_acmd6_clean ||
			     profile == UMS9117_SDIO_CLOCK_HIGH_SPEED)) ||
	    (!first_width && profile == UMS9117_SDIO_CLOCK_HIGH_SPEED &&
	     !priv->width_acmd6_clean))
		return -EPROTO;
	return ta1618_sdio_set_operational_clock(&ta1618_sdio, &priv->state,
						 profile, NULL);
}

static int ta1618_mmc_set_ios(struct udevice *dev)
{
	struct ta1618_mmc_plat *plat = dev_get_plat(dev);
	struct ta1618_mmc_priv *priv = dev_get_priv(dev);
	struct mmc *mmc = &plat->mmc;
	int ret;

	if (mmc->clk_disable) {
		if (priv->state.platform_active ||
		    priv->state.card_detect_owned)
			ta1618_mmc_cleanup(priv);
		return 0;
	}
	if (!priv->state.platform_active)
		return -EIO;
	if (mmc->bus_width == 1U) {
		if (priv->state.controller.physical_width4)
			return -EPROTO;
		return ta1618_sdio_validate_active(&ta1618_sdio, &priv->state);
	}
	if (mmc->bus_width != 4U)
		return -EOPNOTSUPP;
	ret = ta1618_mmc_set_clock(priv, UMS9117_SDIO_CLOCK_LEGACY);
	if (ret)
		goto fail_closed;
	if (mmc->selected_mode == SD_HS &&
	    mmc->clock >= UMS9117_SDIO_HS_CLOCK_HZ) {
		ret = ta1618_mmc_set_clock(priv, UMS9117_SDIO_CLOCK_HIGH_SPEED);
		if (ret)
			goto fail_closed;
	}
	return 0;

fail_closed:
	ta1618_mmc_cleanup(priv);
	return ret;
}

static int ta1618_mmc_get_cd(struct udevice *dev)
{
	struct ta1618_mmc_priv *priv = dev_get_priv(dev);
	int present;
	int ret;

	if (!priv->state.card_detect_owned) {
		ret = ta1618_sdio_enable_card_detect(&ta1618_sdio,
						     &priv->state);
		if (ret) {
			if (priv->state.card_detect_owned ||
			    priv->state.platform_active)
				ta1618_mmc_cleanup(priv);
			return 0;
		}
	}
	present = ta1618_sdio_card_present(&ta1618_sdio, &priv->state);
	if ((present <= 0 && priv->state.platform_active) ||
	    (present < 0 && priv->state.card_detect_owned))
		ta1618_mmc_cleanup(priv);
	return present > 0;
}

static int ta1618_mmc_get_wp(struct udevice *dev)
{
	(void)dev;
	return 1;
}

static int ta1618_mmc_host_power_cycle(struct udevice *dev)
{
	struct ta1618_mmc_priv *priv = dev_get_priv(dev);

	if (priv->state.platform_active || priv->state.card_detect_owned)
		ta1618_mmc_cleanup(priv);
	return 0;
}

static int ta1618_mmc_reinit(struct udevice *dev)
{
	struct ta1618_mmc_priv *priv = dev_get_priv(dev);
	int present;
	int ret;

	if (priv->state.platform_active || priv->state.card_detect_owned)
		ta1618_mmc_cleanup(priv);
	if (!priv->state.snapshots_valid) {
		ret = ta1618_sdio_snapshot(&ta1618_sdio, &priv->state);
		if (ret)
			return ret;
	}
	ret = ta1618_sdio_enable_card_detect(&ta1618_sdio, &priv->state);
	if (ret) {
		if (priv->state.card_detect_owned ||
		    priv->state.platform_active)
			ta1618_mmc_cleanup(priv);
		return ret;
	}
	present = ta1618_sdio_card_present(&ta1618_sdio, &priv->state);
	if (present < 0) {
		ta1618_mmc_cleanup(priv);
		return present;
	}
	if (!present)
		return -ENOMEDIUM;
	ret = ta1618_sdio_activate(&ta1618_sdio, &priv->state);
	if (!ret)
		ret = ta1618_sdio_enable_ident_clock(&ta1618_sdio, &priv->state,
						     NULL);
	if (ret) {
		ta1618_mmc_cleanup(priv);
		return ret;
	}
	ta1618_mmc_reset_protocol_state(priv);
	if (priv->mmc)
		priv->mmc->clk_disable = false;
	return 0;
}

static int ta1618_mmc_get_b_max(struct udevice *dev, void *dst, lbaint_t blocks)
{
	(void)dev;
	(void)dst;
	(void)blocks;
	return 1;
}

static const struct dm_mmc_ops ta1618_mmc_ops = {
	.send_cmd = ta1618_mmc_send_cmd,
	.set_ios = ta1618_mmc_set_ios,
	.get_cd = ta1618_mmc_get_cd,
	.get_wp = ta1618_mmc_get_wp,
	.host_power_cycle = ta1618_mmc_host_power_cycle,
	.reinit = ta1618_mmc_reinit,
	.get_b_max = ta1618_mmc_get_b_max,
};

static int ta1618_mmc_bind(struct udevice *dev)
{
	struct ta1618_mmc_plat *plat = dev_get_plat(dev);
	struct mmc_config *cfg = &plat->cfg;

	cfg->name = dev->name;
	cfg->host_caps = MMC_MODE_1BIT | MMC_MODE_4BIT | MMC_MODE_HS;
	cfg->voltages = MMC_VDD_29_30 | MMC_VDD_30_31;
	cfg->f_min = UMS9117_SDIO_IDENT_CLOCK_HZ;
	cfg->f_max = UMS9117_SDIO_HS_CLOCK_HZ;
	cfg->b_max = 1;
	return mmc_bind(dev, &plat->mmc, cfg);
}

static int ta1618_mmc_probe(struct udevice *dev)
{
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct ta1618_mmc_plat *plat = dev_get_plat(dev);
	struct ta1618_mmc_priv *priv = dev_get_priv(dev);
	int ret;

	ret = ta1618_sdio_snapshot(&ta1618_sdio, &priv->state);
	if (ret)
		return ret;
	ret = ta1618_sdio_enable_card_detect(&ta1618_sdio, &priv->state);
	if (ret) {
		ta1618_mmc_cleanup(priv);
		return ret;
	}
	plat->mmc.priv = priv;
	upriv->mmc = &plat->mmc;
	priv->mmc = &plat->mmc;
	ta1618_mmc_instance = priv;
	return 0;
}

static int ta1618_mmc_remove(struct udevice *dev)
{
	struct ta1618_mmc_priv *priv = dev_get_priv(dev);

	if (priv->state.snapshots_valid)
		ta1618_mmc_cleanup(priv);
	if (ta1618_mmc_instance == priv)
		ta1618_mmc_instance = NULL;
	return 0;
}

void ta1618_mmc_release(void)
{
	if (ta1618_mmc_instance &&
	    (ta1618_mmc_instance->state.platform_active ||
	     ta1618_mmc_instance->state.card_detect_owned))
		ta1618_mmc_cleanup(ta1618_mmc_instance);
	if (ta1618_mmc_instance && ta1618_mmc_instance->mmc) {
		ta1618_mmc_instance->mmc->has_init = 0;
		ta1618_mmc_instance->mmc->init_in_progress = 0;
		ta1618_mmc_instance->mmc->op_cond_pending = 0;
	}
}

static int do_ta1618_sdrelease(struct cmd_tbl *cmdtp, int flag, int argc,
			       char *const argv[])
{
	(void)cmdtp;
	(void)flag;
	(void)argc;
	(void)argv;

	ta1618_mmc_release();
	puts("sdrelease: controller and slot state restored\n");
	return CMD_RET_SUCCESS;
}

static const struct udevice_id ta1618_mmc_ids[] = {
	{ .compatible = "fplinux,ta1618-mmc" },
	{}
};

U_BOOT_DRIVER(ta1618_mmc) = {
	.name = "ta1618_mmc",
	.id = UCLASS_MMC,
	.of_match = ta1618_mmc_ids,
	.bind = ta1618_mmc_bind,
	.probe = ta1618_mmc_probe,
	.remove = ta1618_mmc_remove,
	.ops = &ta1618_mmc_ops,
	.plat_auto = sizeof(struct ta1618_mmc_plat),
	.priv_auto = sizeof(struct ta1618_mmc_priv),
};

U_BOOT_CMD(sdrelease, 1, 0, do_ta1618_sdrelease,
	   "restore the TA-1618 MMC controller and slot baseline", "");
