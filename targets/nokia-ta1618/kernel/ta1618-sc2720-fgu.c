// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/soc/sprd/ums9117-adi.h>

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_MODULE_EN0 0xc08U
#define SC2720_RTC_CLK_EN0 0xc10U
#define SC2720_SOFT_RST0 0xc14U

#define SC2720_EFUSE_GLB_CTRL 0x300U
#define SC2720_EFUSE_DATA_RD 0x304U
#define SC2720_EFUSE_BLOCK_INDEX 0x30cU
#define SC2720_EFUSE_MODE_CTRL 0x310U
#define SC2720_EFUSE_STATUS 0x314U

#define SC2720_FGU_CONFIG 0xa04U
#define SC2720_FGU_ADC_CONFIG 0xa08U
#define SC2720_FGU_STATUS 0xa0cU
#define SC2720_FGU_INT_RAW 0xa18U
#define SC2720_FGU_VOLTAGE 0xa20U
#define SC2720_CHGR_DET_FGU_CTRL 0xe18U

#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U

#define SC2720_MODULE_EN0_EFS BIT(6)
#define SC2720_MODULE_EN0_FGU BIT(7)
#define SC2720_RTC_CLK_EN0_FGU BIT(6)
#define SC2720_RTC_CLK_EN0_EFS BIT(11)
#define SC2720_SOFT_RST0_FGU BIT(4)
#define SC2720_SOFT_RST0_EFS BIT(7)

#define SC2720_EFUSE_BLOCK_INDEX_MASK GENMASK(4, 0)
#define SC2720_EFUSE_MODE_RD_START BIT(1)
#define SC2720_EFUSE_MODE_NORMAL_RD_FLAG_CLR BIT(2)
#define SC2720_EFUSE_STATUS_PGM_BUSY BIT(0)
#define SC2720_EFUSE_STATUS_STANDBY_BUSY BIT(2)
#define SC2720_EFUSE_STATUS_NORMAL_RD_DONE BIT(4)

#define SC2720_CHGR_DET_FGU_ANALOG_MASK GENMASK(13, 12)
#define SC2720_FGU_CONFIG_VOLT_H_VALID BIT(12)
#define SC2720_FGU_CONFIG_DISABLE BIT(11)
#define SC2720_FGU_ADC_CONFIG_RESET BIT(1)
#define SC2720_FGU_ADC_CONFIG_POWER_DOWN BIT(0)
#define SC2720_FGU_STATUS_TOP_SELECTED BIT(6)
#define SC2720_FGU_INT_RAW_VOLTAGE_VALID BIT(6)
#define SC2720_FGU_VOLTAGE_RESERVED GENMASK(15, 12)

#define TA1618_EFUSE3_BLOCK 3U
#define TA1618_EFUSE_POLL_INTERVAL_MS 10U
#define TA1618_EFUSE_POLL_TIMEOUT_MS 3000U
#define TA1618_FGU_TRIM_MASK GENMASK(8, 0)
#define TA1618_FGU_ADC_4200_BASE 2611U
#define TA1618_FGU_CODES_NUMERATOR 10U
#define TA1618_FGU_CODES_DENOMINATOR 42U
#define TA1618_FGU_READY_POLL_MS 100U
#define TA1618_FGU_READY_TIMEOUT_MS 5000U

struct ta1618_fgu {
	struct device *dev;
	u32 codes_per_1000mv;
	bool pclk_owned;
};

struct ta1618_efuse_context {
	u16 block_index;
	bool block_index_saved;
	bool pgm_busy_seen;
	bool unsafe_controller;
};

struct ta1618_fgu_context {
	u16 chip_id_low;
	u16 chip_id_high;
	u16 module_en0;
	u16 rtc_clk_en0;
	u16 soft_rst0;
	u16 chgr_det_fgu_ctrl;
};

struct ta1618_fgu_sample {
	u16 chip_id_low;
	u16 chip_id_high;
	u16 module_en0;
	u16 rtc_clk_en0;
	u16 soft_rst0;
	u16 chgr_det_fgu_ctrl;
	u16 config;
	u16 adc_config;
	u16 status;
	u16 int_raw;
	u16 voltage[3];
};

static int
ta1618_fgu_finish_transaction(struct ums9117_adi_transaction *transaction,
			      int ret)
{
	int end_ret;

	end_ret = ums9117_adi_end(transaction);
	return ret ? ret : end_ret;
}

static int ta1618_fgu_read_if_ok(struct ums9117_adi_transaction *transaction,
				 int ret, u32 offset, u16 *value)
{
	if (ret)
		return ret;

	return ums9117_adi_read(transaction, offset, value);
}

static int ta1618_fgu_read_initial_context_locked(
	struct ums9117_adi_transaction *transaction,
	struct ta1618_fgu_context *context)
{
	int ret;

	ret = ums9117_adi_read(transaction, SC2720_CHIP_ID_LOW,
			       &context->chip_id_low);
	if (!ret)
		ret = ums9117_adi_read(transaction, SC2720_CHIP_ID_HIGH,
				       &context->chip_id_high);
	if (!ret)
		ret = ums9117_adi_read(transaction, SC2720_MODULE_EN0,
				       &context->module_en0);
	if (!ret)
		ret = ums9117_adi_read(transaction, SC2720_RTC_CLK_EN0,
				       &context->rtc_clk_en0);
	if (!ret)
		ret = ums9117_adi_read(transaction, SC2720_SOFT_RST0,
				       &context->soft_rst0);
	if (!ret)
		ret = ums9117_adi_read(transaction, SC2720_CHGR_DET_FGU_CTRL,
				       &context->chgr_det_fgu_ctrl);

	return ret;
}

static int ta1618_fgu_read_initial_context(struct ta1618_fgu_context *context)
{
	struct ums9117_adi_transaction transaction = {};
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_fgu_read_initial_context_locked(&transaction, context);
	ret = ta1618_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;

	return 0;
}

static int
ta1618_fgu_validate_initial_context(const struct ta1618_fgu_context *context)
{
	if (context->chip_id_low != SC2720_EXPECTED_ID_LOW ||
	    context->chip_id_high != SC2720_EXPECTED_ID_HIGH)
		return -ENODEV;
	if (context->module_en0 &
	    (SC2720_MODULE_EN0_EFS | SC2720_MODULE_EN0_FGU))
		return -EBUSY;
	if ((context->rtc_clk_en0 &
	     (SC2720_RTC_CLK_EN0_EFS | SC2720_RTC_CLK_EN0_FGU)) !=
	    (SC2720_RTC_CLK_EN0_EFS | SC2720_RTC_CLK_EN0_FGU))
		return -ENODATA;
	if (context->soft_rst0 & (SC2720_SOFT_RST0_EFS | SC2720_SOFT_RST0_FGU))
		return -ENODATA;
	if (context->chgr_det_fgu_ctrl & SC2720_CHGR_DET_FGU_ANALOG_MASK)
		return -ENODATA;

	return 0;
}

static int ta1618_fgu_efuse_check_guard(struct ta1618_efuse_context *context,
					u16 glb_ctrl, u16 status)
{
	if (status & SC2720_EFUSE_STATUS_PGM_BUSY) {
		context->pgm_busy_seen = true;
		return -EBUSY;
	}
	if (glb_ctrl) {
		context->unsafe_controller = true;
		return -EPERM;
	}

	return 0;
}

static int
ta1618_fgu_efuse_read_guard_locked(struct ums9117_adi_transaction *transaction,
				   struct ta1618_efuse_context *context,
				   u16 *status)
{
	u16 glb_ctrl;
	int ret;

	ret = ums9117_adi_read(transaction, SC2720_EFUSE_GLB_CTRL, &glb_ctrl);
	if (ret)
		return ret;
	ret = ums9117_adi_read(transaction, SC2720_EFUSE_STATUS, status);
	if (ret)
		return ret;

	return ta1618_fgu_efuse_check_guard(context, glb_ctrl, *status);
}

static int ta1618_fgu_efuse_read_guard(struct ta1618_efuse_context *context,
				       u16 *status)
{
	struct ums9117_adi_transaction transaction = {};
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_fgu_efuse_read_guard_locked(&transaction, context, status);
	return ta1618_fgu_finish_transaction(&transaction, ret);
}

static int ta1618_fgu_efuse_enable_gate(bool *attempted)
{
	struct ums9117_adi_transaction transaction = {};
	u16 module_en0;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, SC2720_MODULE_EN0, &module_en0);
	if (!ret && (module_en0 & SC2720_MODULE_EN0_EFS))
		ret = -EBUSY;
	if (!ret) {
		*attempted = true;
		ret = ums9117_adi_update_bits(&transaction, SC2720_MODULE_EN0,
					      SC2720_MODULE_EN0_EFS,
					      SC2720_MODULE_EN0_EFS);
	}
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_MODULE_EN0,
				       &module_en0);
	ret = ta1618_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (!(module_en0 & SC2720_MODULE_EN0_EFS))
		return -EIO;

	return 0;
}

static int
ta1618_fgu_efuse_clear_gate_best_effort(struct ta1618_efuse_context *context,
					bool *cleared)
{
	struct ums9117_adi_transaction transaction = {};
	u16 module_en0;
	u16 status;
	int ret;

	*cleared = false;
	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, SC2720_MODULE_EN0, &module_en0);
	if (!ret)
		*cleared = !(module_en0 & SC2720_MODULE_EN0_EFS);
	if (!ret && !*cleared)
		ret = ta1618_fgu_efuse_read_guard_locked(&transaction, context,
							 &status);
	if (!ret && !*cleared &&
	    (!(status & SC2720_EFUSE_STATUS_STANDBY_BUSY) ||
	     (status & SC2720_EFUSE_STATUS_NORMAL_RD_DONE)))
		ret = -EBUSY;
	if (!ret && !*cleared)
		ret = ums9117_adi_update_bits(&transaction, SC2720_MODULE_EN0,
					      SC2720_MODULE_EN0_EFS, 0);
	if (!ret && !*cleared)
		ret = ums9117_adi_read(&transaction, SC2720_MODULE_EN0,
				       &module_en0);
	if (!ret && !*cleared)
		*cleared = !(module_en0 & SC2720_MODULE_EN0_EFS);
	ret = ta1618_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (!*cleared)
		return -EIO;

	return 0;
}

static int
ta1618_fgu_efuse_save_block_index(struct ta1618_efuse_context *context)
{
	struct ums9117_adi_transaction transaction = {};
	u16 block_index_repeat;
	u16 status;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_fgu_efuse_read_guard_locked(&transaction, context,
						 &status);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_EFUSE_BLOCK_INDEX,
				       &context->block_index);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_EFUSE_BLOCK_INDEX,
				       &block_index_repeat);
	ret = ta1618_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (context->block_index != block_index_repeat ||
	    context->block_index & ~SC2720_EFUSE_BLOCK_INDEX_MASK)
		return -EAGAIN;

	context->block_index_saved = true;
	return 0;
}

static int ta1618_fgu_efuse_mode_command(struct ta1618_efuse_context *context,
					 u16 command)
{
	struct ums9117_adi_transaction transaction = {};
	u16 status;
	int ret;

	if (command != SC2720_EFUSE_MODE_RD_START &&
	    command != SC2720_EFUSE_MODE_NORMAL_RD_FLAG_CLR)
		return -EINVAL;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_fgu_efuse_read_guard_locked(&transaction, context,
						 &status);
	if (!ret)
		ret = ums9117_adi_write_final(&transaction,
					      SC2720_EFUSE_MODE_CTRL, command);
	return ta1618_fgu_finish_transaction(&transaction, ret);
}

static int
ta1618_fgu_efuse_wait_for_status(struct ta1618_efuse_context *context,
				 u16 required, u16 forbidden)
{
	unsigned long deadline;
	u16 status;
	int ret;

	deadline = jiffies + msecs_to_jiffies(TA1618_EFUSE_POLL_TIMEOUT_MS);
	for (;;) {
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		msleep(TA1618_EFUSE_POLL_INTERVAL_MS);
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		ret = ta1618_fgu_efuse_read_guard(context, &status);
		if (ret)
			return ret;
		if ((status & required) == required && !(status & forbidden))
			return 0;
	}
}

static int ta1618_fgu_efuse_select_block(struct ta1618_efuse_context *context,
					 u16 block_index)
{
	struct ums9117_adi_transaction transaction = {};
	u16 status;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_fgu_efuse_read_guard_locked(&transaction, context,
						 &status);
	if (!ret && (!(status & SC2720_EFUSE_STATUS_STANDBY_BUSY) ||
		     (status & SC2720_EFUSE_STATUS_NORMAL_RD_DONE)))
		ret = -EBUSY;
	if (!ret)
		ret = ums9117_adi_write(&transaction, SC2720_EFUSE_BLOCK_INDEX,
					block_index);
	return ta1618_fgu_finish_transaction(&transaction, ret);
}

static int ta1618_fgu_efuse_read_data(struct ta1618_efuse_context *context,
				      u16 *value)
{
	struct ums9117_adi_transaction transaction = {};
	u16 status;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_fgu_efuse_read_guard_locked(&transaction, context,
						 &status);
	if (!ret && !(status & SC2720_EFUSE_STATUS_NORMAL_RD_DONE))
		ret = -EIO;
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_EFUSE_DATA_RD,
				       value);
	return ta1618_fgu_finish_transaction(&transaction, ret);
}

static int ta1618_fgu_efuse_cleanup(struct ta1618_efuse_context *context,
				    bool read_start_may_have_issued,
				    bool normal_read_complete,
				    bool *gate_cleared)
{
	int cleanup_ret = 0;
	int ret;

	if (context->unsafe_controller)
		return -EPERM;
	if (read_start_may_have_issued) {
		if (!normal_read_complete) {
			ret = ta1618_fgu_efuse_wait_for_status(
				context, SC2720_EFUSE_STATUS_STANDBY_BUSY, 0);
			if (ret)
				return ret;
		}
		ret = ta1618_fgu_efuse_mode_command(
			context, SC2720_EFUSE_MODE_NORMAL_RD_FLAG_CLR);
		if (ret)
			return ret;
		ret = ta1618_fgu_efuse_wait_for_status(
			context, SC2720_EFUSE_STATUS_STANDBY_BUSY,
			SC2720_EFUSE_STATUS_NORMAL_RD_DONE);
		if (ret)
			return ret;
	}
	if (!context->block_index_saved)
		return ta1618_fgu_efuse_clear_gate_best_effort(context,
							       gate_cleared);
	ret = ta1618_fgu_efuse_select_block(context, context->block_index);
	if (ret)
		cleanup_ret = ret;
	if (context->pgm_busy_seen || context->unsafe_controller)
		return cleanup_ret;
	if (cleanup_ret)
		return cleanup_ret;

	ret = ta1618_fgu_efuse_clear_gate_best_effort(context, gate_cleared);
	if (ret)
		cleanup_ret = ret;

	return cleanup_ret;
}

static int ta1618_fgu_read_efuse3(struct device *dev, u16 *value)
{
	struct ta1618_efuse_context context = {};
	struct ta1618_fgu_context initial_context = {};
	bool gate_attempted = false;
	bool gate_cleared;
	bool read_start_may_have_issued = false;
	int cleanup_ret;
	int ret;

	ret = ta1618_fgu_read_initial_context(&initial_context);
	if (ret)
		return ret;
	ret = ta1618_fgu_validate_initial_context(&initial_context);
	if (ret)
		return ret;

	ret = ta1618_fgu_efuse_enable_gate(&gate_attempted);
	if (ret)
		goto fail;
	ret = ta1618_fgu_efuse_save_block_index(&context);
	if (ret)
		goto fail;
	ret = ta1618_fgu_efuse_mode_command(
		&context, SC2720_EFUSE_MODE_NORMAL_RD_FLAG_CLR);
	if (ret)
		goto fail;
	ret = ta1618_fgu_efuse_wait_for_status(
		&context, SC2720_EFUSE_STATUS_STANDBY_BUSY,
		SC2720_EFUSE_STATUS_NORMAL_RD_DONE);
	if (ret)
		goto fail;
	ret = ta1618_fgu_efuse_select_block(&context, TA1618_EFUSE3_BLOCK);
	if (ret)
		goto fail;
	read_start_may_have_issued = true;
	ret = ta1618_fgu_efuse_mode_command(&context,
					    SC2720_EFUSE_MODE_RD_START);
	if (ret)
		goto fail;
	ret = ta1618_fgu_efuse_wait_for_status(
		&context, SC2720_EFUSE_STATUS_NORMAL_RD_DONE, 0);
	if (ret)
		goto fail;
	ret = ta1618_fgu_efuse_read_data(&context, value);
	if (ret)
		goto fail;
	ret = ta1618_fgu_efuse_cleanup(&context, true, true, &gate_cleared);
	if (!ret)
		return 0;
	if (context.pgm_busy_seen)
		dev_err(dev, "efuse program busy; EFS clock remains enabled\n");
	else if (context.unsafe_controller)
		dev_err(dev,
			"efuse controller active; EFS clock remains enabled\n");
	else
		dev_err(dev, "efuse cleanup failed: %d\n", ret);
	return ret;

fail:
	if (context.pgm_busy_seen) {
		if (gate_attempted)
			dev_err(dev,
				"efuse program busy; EFS clock remains enabled\n");
		return ret;
	}
	if (context.unsafe_controller) {
		if (gate_attempted)
			dev_err(dev,
				"efuse controller active; EFS clock remains enabled\n");
		return ret;
	}
	if (!gate_attempted)
		return ret;

	cleanup_ret = ta1618_fgu_efuse_cleanup(
		&context, read_start_may_have_issued, false, &gate_cleared);
	if (context.pgm_busy_seen)
		dev_err(dev, "efuse program busy; EFS clock remains enabled\n");
	else if (context.unsafe_controller)
		dev_err(dev,
			"efuse controller active; EFS clock remains enabled\n");
	else if (cleanup_ret)
		dev_err(dev, "efuse cleanup after %d failed: %d\n", ret,
			cleanup_ret);

	return ret;
}

static int ta1618_fgu_enable_pclk(bool *attempted)
{
	struct ums9117_adi_transaction transaction = {};
	struct ta1618_fgu_context context = {};
	int ret;

	*attempted = false;
	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_fgu_read_initial_context_locked(&transaction, &context);
	if (!ret)
		ret = ta1618_fgu_validate_initial_context(&context);
	if (!ret) {
		*attempted = true;
		ret = ums9117_adi_update_bits(&transaction, SC2720_MODULE_EN0,
					      SC2720_MODULE_EN0_FGU,
					      SC2720_MODULE_EN0_FGU);
	}
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_MODULE_EN0,
				       &context.module_en0);
	ret = ta1618_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (!(context.module_en0 & SC2720_MODULE_EN0_FGU))
		return -EIO;

	return 0;
}

static int ta1618_fgu_clear_pclk_best_effort(bool *cleared)
{
	struct ums9117_adi_transaction transaction = {};
	u16 module_en0;
	int ret;

	*cleared = false;
	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, SC2720_MODULE_EN0, &module_en0);
	if (!ret)
		*cleared = !(module_en0 & SC2720_MODULE_EN0_FGU);
	if (!ret && !*cleared)
		ret = ums9117_adi_update_bits(&transaction, SC2720_MODULE_EN0,
					      SC2720_MODULE_EN0_FGU, 0);
	if (!ret && !*cleared)
		ret = ums9117_adi_read(&transaction, SC2720_MODULE_EN0,
				       &module_en0);
	if (!ret && !*cleared)
		*cleared = !(module_en0 & SC2720_MODULE_EN0_FGU);
	ret = ta1618_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (!*cleared)
		return -EIO;

	return 0;
}

static void ta1618_fgu_release_pclk(void *data)
{
	struct ta1618_fgu *fgu = data;
	bool cleared;
	int ret;

	if (!fgu->pclk_owned)
		return;
	ret = ta1618_fgu_clear_pclk_best_effort(&cleared);
	if (ret || !cleared) {
		dev_err(fgu->dev, "failed to release FGU clock: %d\n",
			ret ? ret : -EIO);
		return;
	}
	fgu->pclk_owned = false;
}

static int ta1618_fgu_calibrate(struct ta1618_fgu *fgu, u16 efuse3)
{
	u32 adc_4200;
	u32 codes_per_1000mv;

	adc_4200 = (efuse3 & TA1618_FGU_TRIM_MASK) + TA1618_FGU_ADC_4200_BASE;
	codes_per_1000mv =
		DIV_ROUND_CLOSEST(adc_4200 * TA1618_FGU_CODES_NUMERATOR,
				  TA1618_FGU_CODES_DENOMINATOR);
	if (!codes_per_1000mv)
		return -ERANGE;

	fgu->codes_per_1000mv = codes_per_1000mv;
	return 0;
}

static u16 ta1618_fgu_median3(u16 a, u16 b, u16 c)
{
	if (a > b)
		swap(a, b);
	if (b > c)
		swap(b, c);
	if (a > b)
		swap(a, b);

	return b;
}

static int ta1618_fgu_read_sample(struct ta1618_fgu_sample *sample)
{
	struct ums9117_adi_transaction transaction = {};
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_CHIP_ID_LOW,
				    &sample->chip_id_low);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_CHIP_ID_HIGH,
				    &sample->chip_id_high);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_MODULE_EN0,
				    &sample->module_en0);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_RTC_CLK_EN0,
				    &sample->rtc_clk_en0);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_SOFT_RST0,
				    &sample->soft_rst0);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_CHGR_DET_FGU_CTRL,
				    &sample->chgr_det_fgu_ctrl);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_FGU_CONFIG,
				    &sample->config);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_FGU_ADC_CONFIG,
				    &sample->adc_config);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_FGU_STATUS,
				    &sample->status);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_FGU_INT_RAW,
				    &sample->int_raw);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_FGU_VOLTAGE,
				    &sample->voltage[0]);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_FGU_VOLTAGE,
				    &sample->voltage[1]);
	ret = ta1618_fgu_read_if_ok(&transaction, ret, SC2720_FGU_VOLTAGE,
				    &sample->voltage[2]);

	return ta1618_fgu_finish_transaction(&transaction, ret);
}

static int
ta1618_fgu_sample_to_microvolt(const struct ta1618_fgu *fgu,
			       const struct ta1618_fgu_sample *sample,
			       int *microvolt)
{
	u16 voltage_raw;
	u32 voltage_mv;

	if (sample->chip_id_low != SC2720_EXPECTED_ID_LOW ||
	    sample->chip_id_high != SC2720_EXPECTED_ID_HIGH)
		return -ENODEV;
	if (!(sample->module_en0 & SC2720_MODULE_EN0_FGU))
		return -EIO;
	if (!(sample->rtc_clk_en0 & SC2720_RTC_CLK_EN0_FGU) ||
	    sample->soft_rst0 & SC2720_SOFT_RST0_FGU ||
	    sample->chgr_det_fgu_ctrl & SC2720_CHGR_DET_FGU_ANALOG_MASK)
		return -ENODATA;
	if (sample->config & (SC2720_FGU_CONFIG_VOLT_H_VALID |
			      SC2720_FGU_CONFIG_DISABLE) ||
	    sample->adc_config & (SC2720_FGU_ADC_CONFIG_RESET |
				  SC2720_FGU_ADC_CONFIG_POWER_DOWN) ||
	    !(sample->status & SC2720_FGU_STATUS_TOP_SELECTED) ||
	    !(sample->int_raw & SC2720_FGU_INT_RAW_VOLTAGE_VALID))
		return -ENODATA;
	if (!sample->voltage[0] || !sample->voltage[1] || !sample->voltage[2] ||
	    (sample->voltage[0] | sample->voltage[1] | sample->voltage[2]) &
		    SC2720_FGU_VOLTAGE_RESERVED)
		return -ENODATA;

	voltage_raw = ta1618_fgu_median3(sample->voltage[0], sample->voltage[1],
					 sample->voltage[2]);
	voltage_mv = DIV_ROUND_CLOSEST((u32)voltage_raw * 1000U,
				       fgu->codes_per_1000mv);
	*microvolt = voltage_mv * 1000U;

	return 0;
}

static int ta1618_fgu_wait_usable(const struct ta1618_fgu *fgu)
{
	struct ta1618_fgu_sample sample = {};
	unsigned int waited;
	int microvolt;
	int ret;

	for (waited = 0; waited <= TA1618_FGU_READY_TIMEOUT_MS;
	     waited += TA1618_FGU_READY_POLL_MS) {
		ret = ta1618_fgu_read_sample(&sample);
		if (ret)
			return ret;
		ret = ta1618_fgu_sample_to_microvolt(fgu, &sample, &microvolt);
		if (!ret)
			return 0;
		if (ret != -ENODATA)
			return ret;
		if (waited != TA1618_FGU_READY_TIMEOUT_MS)
			msleep(TA1618_FGU_READY_POLL_MS);
	}

	return -ETIMEDOUT;
}

static int ta1618_fgu_get_property(struct power_supply *supply,
				   enum power_supply_property property,
				   union power_supply_propval *value)
{
	struct ta1618_fgu *fgu = power_supply_get_drvdata(supply);
	struct ta1618_fgu_sample sample = {};
	int ret;

	if (property != POWER_SUPPLY_PROP_VOLTAGE_NOW)
		return -EINVAL;
	ret = ta1618_fgu_read_sample(&sample);
	if (ret)
		return ret;

	return ta1618_fgu_sample_to_microvolt(fgu, &sample, &value->intval);
}

static enum power_supply_property ta1618_fgu_properties[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static const struct power_supply_desc ta1618_fgu_description = {
	.name = "ta1618-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = ta1618_fgu_properties,
	.num_properties = ARRAY_SIZE(ta1618_fgu_properties),
	.get_property = ta1618_fgu_get_property,
};

static int ta1618_fgu_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	struct power_supply *supply;
	struct ta1618_fgu *fgu;
	bool pclk_attempted;
	bool pclk_cleared;
	int cleanup_ret;
	u16 efuse3;
	int ret;

	fgu = devm_kzalloc(&pdev->dev, sizeof(*fgu), GFP_KERNEL);
	if (!fgu)
		return -ENOMEM;
	fgu->dev = &pdev->dev;

	ret = ta1618_fgu_read_efuse3(&pdev->dev, &efuse3);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 efuse calibration unavailable\n");
	ret = ta1618_fgu_calibrate(fgu, efuse3);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 efuse calibration invalid\n");

	ret = ta1618_fgu_enable_pclk(&pclk_attempted);
	if (ret) {
		if (pclk_attempted) {
			cleanup_ret = ta1618_fgu_clear_pclk_best_effort(
				&pclk_cleared);
			if (cleanup_ret || !pclk_cleared)
				dev_err(&pdev->dev,
					"FGU clock cleanup after %d failed: %d\n",
					ret, cleanup_ret);
		}
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 FGU clock unavailable\n");
	}
	fgu->pclk_owned = true;
	ret = devm_add_action_or_reset(&pdev->dev, ta1618_fgu_release_pclk,
				       fgu);
	if (ret)
		return ret;
	ret = ta1618_fgu_wait_usable(fgu);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 voltage sample unavailable\n");

	config.drv_data = fgu;
	config.fwnode = dev_fwnode(&pdev->dev);
	supply = devm_power_supply_register(&pdev->dev, &ta1618_fgu_description,
					    &config);
	return PTR_ERR_OR_ZERO(supply);
}

static const struct of_device_id ta1618_fgu_of_match[] = {
	{ .compatible = "fplinux,ta1618-sc2720-fgu" },
	{},
};
MODULE_DEVICE_TABLE(of, ta1618_fgu_of_match);

static struct platform_driver ta1618_fgu_driver = {
	.probe = ta1618_fgu_probe,
	.driver = {
		.name = "ta1618-sc2720-fgu",
		.of_match_table = ta1618_fgu_of_match,
	},
};
module_platform_driver(ta1618_fgu_driver);

MODULE_DESCRIPTION("Nokia TA-1618 read-only SC2720 battery voltage");
MODULE_LICENSE("GPL");
