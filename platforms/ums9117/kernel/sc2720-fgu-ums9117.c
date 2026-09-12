// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
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
#define SC2720_FGU_CURRENT 0xa2cU
#define SC2720_FGU_CLBCNT_VALUE_HIGH 0xa68U
#define SC2720_FGU_CLBCNT_VALUE_LOW 0xa6cU
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
#define SC2720_FGU_CONFIG_SW_DIS_CURT BIT(3)
#define SC2720_FGU_ADC_CONFIG_RESET BIT(1)
#define SC2720_FGU_ADC_CONFIG_POWER_DOWN BIT(0)
#define SC2720_FGU_ADC_CONFIG_SOFTWARE_FORCE_MASK GENMASK(7, 4)
#define SC2720_FGU_STATUS_TOP_SELECTED BIT(6)
#define SC2720_FGU_INT_RAW_VOLTAGE_VALID BIT(6)
#define SC2720_FGU_INT_RAW_CURRENT_VALID BIT(7)
#define SC2720_FGU_VOLTAGE_RESERVED GENMASK(15, 12)
#define SC2720_FGU_CURRENT_RESERVED GENMASK(15, 14)
#define SC2720_FGU_CURRENT_COUNTS_MASK GENMASK(13, 0)
#define SC2720_FGU_CURRENT_ZERO 0x2000U
#define SC2720_FGU_CLBCNT_SIGN_EXTENSION GENMASK(31, 29)
#define SC2720_FGU_CLBCNT_SIGN_BIT 29U

#define SC2720_EFUSE3_BLOCK 3U
#define SC2720_EFUSE_POLL_INTERVAL_MS 10U
#define SC2720_EFUSE_POLL_TIMEOUT_MS 3000U
#define SC2720_FGU_TRIM_MASK GENMASK(8, 0)
#define SC2720_FGU_ADC_4200_BASE 2611U
#define SC2720_FGU_CODES_NUMERATOR 10U
#define SC2720_FGU_CODES_DENOMINATOR 42U
#define SC2720_FGU_CURRENT_CALIBRATION_FACTOR 4U
#define SC2720_FGU_READY_POLL_MS 100U
#define SC2720_FGU_READY_TIMEOUT_MS 5000U
#define SC2720_FGU_CLBCNT_READ_ATTEMPTS 3U
#define SC2720_FGU_CLBCNT_SAMPLE_HZ 2U
#define SC2720_FGU_MICROAMPS_PER_AMP 1000000ULL
#define SC2720_FGU_SECONDS_PER_HOUR 3600U

struct sc2720_fgu {
	struct device *dev;
	struct power_supply_desc description;
	u32 codes_per_1000mv;
	u32 codes_per_1000ma;
	bool pclk_owned;
};

struct sc2720_efuse_context {
	u16 block_index;
	bool block_index_saved;
	bool pgm_busy_seen;
	bool unsafe_controller;
};

struct sc2720_fgu_context {
	u16 chip_id_low;
	u16 chip_id_high;
	u16 module_en0;
	u16 rtc_clk_en0;
	u16 soft_rst0;
	u16 chgr_det_fgu_ctrl;
};

struct sc2720_fgu_sample {
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
	u16 current_raw;
};

struct sc2720_fgu_charge_sample {
	struct sc2720_fgu_sample state;
	u16 high_before;
	u16 low;
	u16 high_after;
};

static int
sc2720_fgu_finish_transaction(struct ums9117_adi_transaction *transaction,
			      int ret)
{
	int end_ret;

	end_ret = ums9117_adi_end(transaction);
	return ret ? ret : end_ret;
}

static int sc2720_fgu_read_if_ok(struct ums9117_adi_transaction *transaction,
				 int ret, u32 offset, u16 *value)
{
	if (ret)
		return ret;

	return ums9117_adi_read(transaction, offset, value);
}

static int sc2720_fgu_read_initial_context_locked(
	struct ums9117_adi_transaction *transaction,
	struct sc2720_fgu_context *context)
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

static int sc2720_fgu_read_initial_context(struct sc2720_fgu_context *context)
{
	struct ums9117_adi_transaction transaction = {};
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = sc2720_fgu_read_initial_context_locked(&transaction, context);
	ret = sc2720_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;

	return 0;
}

static int
sc2720_fgu_validate_initial_context(const struct sc2720_fgu_context *context)
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

static int sc2720_fgu_efuse_check_guard(struct sc2720_efuse_context *context,
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
sc2720_fgu_efuse_read_guard_locked(struct ums9117_adi_transaction *transaction,
				   struct sc2720_efuse_context *context,
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

	return sc2720_fgu_efuse_check_guard(context, glb_ctrl, *status);
}

static int sc2720_fgu_efuse_read_guard(struct sc2720_efuse_context *context,
				       u16 *status)
{
	struct ums9117_adi_transaction transaction = {};
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = sc2720_fgu_efuse_read_guard_locked(&transaction, context, status);
	return sc2720_fgu_finish_transaction(&transaction, ret);
}

static int sc2720_fgu_efuse_enable_gate(bool *attempted)
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
	ret = sc2720_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (!(module_en0 & SC2720_MODULE_EN0_EFS))
		return -EIO;

	return 0;
}

static int
sc2720_fgu_efuse_clear_gate_best_effort(struct sc2720_efuse_context *context,
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
		ret = sc2720_fgu_efuse_read_guard_locked(&transaction, context,
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
	ret = sc2720_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (!*cleared)
		return -EIO;

	return 0;
}

static int
sc2720_fgu_efuse_save_block_index(struct sc2720_efuse_context *context)
{
	struct ums9117_adi_transaction transaction = {};
	u16 block_index_repeat;
	u16 status;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = sc2720_fgu_efuse_read_guard_locked(&transaction, context,
						 &status);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_EFUSE_BLOCK_INDEX,
				       &context->block_index);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_EFUSE_BLOCK_INDEX,
				       &block_index_repeat);
	ret = sc2720_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (context->block_index != block_index_repeat ||
	    context->block_index & ~SC2720_EFUSE_BLOCK_INDEX_MASK)
		return -EAGAIN;

	context->block_index_saved = true;
	return 0;
}

static int sc2720_fgu_efuse_mode_command(struct sc2720_efuse_context *context,
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
	ret = sc2720_fgu_efuse_read_guard_locked(&transaction, context,
						 &status);
	if (!ret)
		ret = ums9117_adi_write_final(&transaction,
					      SC2720_EFUSE_MODE_CTRL, command);
	return sc2720_fgu_finish_transaction(&transaction, ret);
}

static int
sc2720_fgu_efuse_wait_for_status(struct sc2720_efuse_context *context,
				 u16 required, u16 forbidden)
{
	unsigned long deadline;
	u16 status;
	int ret;

	deadline = jiffies + msecs_to_jiffies(SC2720_EFUSE_POLL_TIMEOUT_MS);
	for (;;) {
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		msleep(SC2720_EFUSE_POLL_INTERVAL_MS);
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		ret = sc2720_fgu_efuse_read_guard(context, &status);
		if (ret)
			return ret;
		if ((status & required) == required && !(status & forbidden))
			return 0;
	}
}

static int sc2720_fgu_efuse_select_block(struct sc2720_efuse_context *context,
					 u16 block_index)
{
	struct ums9117_adi_transaction transaction = {};
	u16 status;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = sc2720_fgu_efuse_read_guard_locked(&transaction, context,
						 &status);
	if (!ret && (!(status & SC2720_EFUSE_STATUS_STANDBY_BUSY) ||
		     (status & SC2720_EFUSE_STATUS_NORMAL_RD_DONE)))
		ret = -EBUSY;
	if (!ret)
		ret = ums9117_adi_write(&transaction, SC2720_EFUSE_BLOCK_INDEX,
					block_index);
	return sc2720_fgu_finish_transaction(&transaction, ret);
}

static int sc2720_fgu_efuse_read_data(struct sc2720_efuse_context *context,
				      u16 *value)
{
	struct ums9117_adi_transaction transaction = {};
	u16 status;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = sc2720_fgu_efuse_read_guard_locked(&transaction, context,
						 &status);
	if (!ret && !(status & SC2720_EFUSE_STATUS_NORMAL_RD_DONE))
		ret = -EIO;
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_EFUSE_DATA_RD,
				       value);
	return sc2720_fgu_finish_transaction(&transaction, ret);
}

static int sc2720_fgu_efuse_cleanup(struct sc2720_efuse_context *context,
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
			ret = sc2720_fgu_efuse_wait_for_status(
				context, SC2720_EFUSE_STATUS_STANDBY_BUSY, 0);
			if (ret)
				return ret;
		}
		ret = sc2720_fgu_efuse_mode_command(
			context, SC2720_EFUSE_MODE_NORMAL_RD_FLAG_CLR);
		if (ret)
			return ret;
		ret = sc2720_fgu_efuse_wait_for_status(
			context, SC2720_EFUSE_STATUS_STANDBY_BUSY,
			SC2720_EFUSE_STATUS_NORMAL_RD_DONE);
		if (ret)
			return ret;
	}
	if (!context->block_index_saved)
		return sc2720_fgu_efuse_clear_gate_best_effort(context,
							       gate_cleared);
	ret = sc2720_fgu_efuse_select_block(context, context->block_index);
	if (ret)
		cleanup_ret = ret;
	if (context->pgm_busy_seen || context->unsafe_controller)
		return cleanup_ret;
	if (cleanup_ret)
		return cleanup_ret;

	ret = sc2720_fgu_efuse_clear_gate_best_effort(context, gate_cleared);
	if (ret)
		cleanup_ret = ret;

	return cleanup_ret;
}

static int sc2720_fgu_read_efuse3(struct device *dev, u16 *value)
{
	struct sc2720_efuse_context context = {};
	struct sc2720_fgu_context initial_context = {};
	bool gate_attempted = false;
	bool gate_cleared;
	bool read_start_may_have_issued = false;
	int cleanup_ret;
	int ret;

	ret = sc2720_fgu_read_initial_context(&initial_context);
	if (ret)
		return ret;
	ret = sc2720_fgu_validate_initial_context(&initial_context);
	if (ret)
		return ret;

	ret = sc2720_fgu_efuse_enable_gate(&gate_attempted);
	if (ret)
		goto fail;
	ret = sc2720_fgu_efuse_save_block_index(&context);
	if (ret)
		goto fail;
	ret = sc2720_fgu_efuse_mode_command(
		&context, SC2720_EFUSE_MODE_NORMAL_RD_FLAG_CLR);
	if (ret)
		goto fail;
	ret = sc2720_fgu_efuse_wait_for_status(
		&context, SC2720_EFUSE_STATUS_STANDBY_BUSY,
		SC2720_EFUSE_STATUS_NORMAL_RD_DONE);
	if (ret)
		goto fail;
	ret = sc2720_fgu_efuse_select_block(&context, SC2720_EFUSE3_BLOCK);
	if (ret)
		goto fail;
	read_start_may_have_issued = true;
	ret = sc2720_fgu_efuse_mode_command(&context,
					    SC2720_EFUSE_MODE_RD_START);
	if (ret)
		goto fail;
	ret = sc2720_fgu_efuse_wait_for_status(
		&context, SC2720_EFUSE_STATUS_NORMAL_RD_DONE, 0);
	if (ret)
		goto fail;
	ret = sc2720_fgu_efuse_read_data(&context, value);
	if (ret)
		goto fail;
	ret = sc2720_fgu_efuse_cleanup(&context, true, true, &gate_cleared);
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

	cleanup_ret = sc2720_fgu_efuse_cleanup(
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

static int sc2720_fgu_enable_pclk(bool *attempted)
{
	struct ums9117_adi_transaction transaction = {};
	struct sc2720_fgu_context context = {};
	int ret;

	*attempted = false;
	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = sc2720_fgu_read_initial_context_locked(&transaction, &context);
	if (!ret)
		ret = sc2720_fgu_validate_initial_context(&context);
	if (!ret) {
		*attempted = true;
		ret = ums9117_adi_update_bits(&transaction, SC2720_MODULE_EN0,
					      SC2720_MODULE_EN0_FGU,
					      SC2720_MODULE_EN0_FGU);
	}
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_MODULE_EN0,
				       &context.module_en0);
	ret = sc2720_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (!(context.module_en0 & SC2720_MODULE_EN0_FGU))
		return -EIO;

	return 0;
}

static int sc2720_fgu_clear_pclk_best_effort(bool *cleared)
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
	ret = sc2720_fgu_finish_transaction(&transaction, ret);
	if (ret)
		return ret;
	if (!*cleared)
		return -EIO;

	return 0;
}

static void sc2720_fgu_release_pclk(void *data)
{
	struct sc2720_fgu *fgu = data;
	bool cleared;
	int ret;

	if (!fgu->pclk_owned)
		return;
	ret = sc2720_fgu_clear_pclk_best_effort(&cleared);
	if (ret || !cleared) {
		dev_err(fgu->dev, "failed to release FGU clock: %d\n",
			ret ? ret : -EIO);
		return;
	}
	fgu->pclk_owned = false;
}

static int sc2720_fgu_calibrate(struct sc2720_fgu *fgu, u16 efuse3,
				u32 calibration_real, u32 calibration_spec)
{
	u32 adc_4200;
	u32 current_codes;
	u32 current_ratio_product;
	u32 codes_per_1000mv;
	u32 codes_per_1000ma;

	adc_4200 = (efuse3 & SC2720_FGU_TRIM_MASK) + SC2720_FGU_ADC_4200_BASE;
	codes_per_1000mv =
		DIV_ROUND_CLOSEST(adc_4200 * SC2720_FGU_CODES_NUMERATOR,
				  SC2720_FGU_CODES_DENOMINATOR);
	if (!codes_per_1000mv)
		return -ERANGE;
	if (check_mul_overflow(codes_per_1000mv,
			       SC2720_FGU_CURRENT_CALIBRATION_FACTOR,
			       &current_ratio_product) ||
	    check_mul_overflow(current_ratio_product, calibration_real,
			       &current_codes))
		return -ERANGE;
	codes_per_1000ma =
		DIV_ROUND_CLOSEST_ULL((u64)current_codes, calibration_spec);
	if (!codes_per_1000ma)
		return -ERANGE;

	fgu->codes_per_1000mv = codes_per_1000mv;
	fgu->codes_per_1000ma = codes_per_1000ma;
	return 0;
}

static u16 sc2720_fgu_median3(u16 a, u16 b, u16 c)
{
	if (a > b)
		swap(a, b);
	if (b > c)
		swap(b, c);
	if (a > b)
		swap(a, b);

	return b;
}

static int sc2720_fgu_read_runtime_state_locked(
	struct ums9117_adi_transaction *transaction,
	struct sc2720_fgu_sample *sample)
{
	int ret = 0;

	ret = sc2720_fgu_read_if_ok(transaction, ret, SC2720_CHIP_ID_LOW,
				    &sample->chip_id_low);
	ret = sc2720_fgu_read_if_ok(transaction, ret, SC2720_CHIP_ID_HIGH,
				    &sample->chip_id_high);
	ret = sc2720_fgu_read_if_ok(transaction, ret, SC2720_MODULE_EN0,
				    &sample->module_en0);
	ret = sc2720_fgu_read_if_ok(transaction, ret, SC2720_RTC_CLK_EN0,
				    &sample->rtc_clk_en0);
	ret = sc2720_fgu_read_if_ok(transaction, ret, SC2720_SOFT_RST0,
				    &sample->soft_rst0);
	ret = sc2720_fgu_read_if_ok(transaction, ret, SC2720_CHGR_DET_FGU_CTRL,
				    &sample->chgr_det_fgu_ctrl);
	ret = sc2720_fgu_read_if_ok(transaction, ret, SC2720_FGU_CONFIG,
				    &sample->config);
	ret = sc2720_fgu_read_if_ok(transaction, ret, SC2720_FGU_ADC_CONFIG,
				    &sample->adc_config);
	ret = sc2720_fgu_read_if_ok(transaction, ret, SC2720_FGU_STATUS,
				    &sample->status);
	return sc2720_fgu_read_if_ok(transaction, ret, SC2720_FGU_INT_RAW,
				     &sample->int_raw);
}

static int sc2720_fgu_read_sample(struct sc2720_fgu_sample *sample)
{
	struct ums9117_adi_transaction transaction = {};
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = sc2720_fgu_read_runtime_state_locked(&transaction, sample);
	ret = sc2720_fgu_read_if_ok(&transaction, ret, SC2720_FGU_VOLTAGE,
				    &sample->voltage[0]);
	ret = sc2720_fgu_read_if_ok(&transaction, ret, SC2720_FGU_VOLTAGE,
				    &sample->voltage[1]);
	ret = sc2720_fgu_read_if_ok(&transaction, ret, SC2720_FGU_VOLTAGE,
				    &sample->voltage[2]);
	ret = sc2720_fgu_read_if_ok(&transaction, ret, SC2720_FGU_CURRENT,
				    &sample->current_raw);

	return sc2720_fgu_finish_transaction(&transaction, ret);
}

static int
sc2720_fgu_read_charge_sample(struct sc2720_fgu_charge_sample *sample)
{
	struct ums9117_adi_transaction transaction = {};
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < SC2720_FGU_CLBCNT_READ_ATTEMPTS;
	     attempt++) {
		ret = ums9117_adi_begin(&transaction);
		if (ret)
			return ret;
		ret = sc2720_fgu_read_runtime_state_locked(&transaction,
							   &sample->state);
		ret = sc2720_fgu_read_if_ok(&transaction, ret,
					    SC2720_FGU_CURRENT,
					    &sample->state.current_raw);
		ret = sc2720_fgu_read_if_ok(&transaction, ret,
					    SC2720_FGU_CLBCNT_VALUE_HIGH,
					    &sample->high_before);
		ret = sc2720_fgu_read_if_ok(&transaction, ret,
					    SC2720_FGU_CLBCNT_VALUE_LOW,
					    &sample->low);
		ret = sc2720_fgu_read_if_ok(&transaction, ret,
					    SC2720_FGU_CLBCNT_VALUE_HIGH,
					    &sample->high_after);
		ret = sc2720_fgu_finish_transaction(&transaction, ret);
		if (ret)
			return ret;
		if (sample->high_before == sample->high_after)
			return 0;
	}

	return -EAGAIN;
}

static int
sc2720_fgu_sample_to_microvolt(const struct sc2720_fgu *fgu,
			       const struct sc2720_fgu_sample *sample,
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

	voltage_raw = sc2720_fgu_median3(sample->voltage[0], sample->voltage[1],
					 sample->voltage[2]);
	voltage_mv = DIV_ROUND_CLOSEST((u32)voltage_raw * 1000U,
				       fgu->codes_per_1000mv);
	*microvolt = voltage_mv * 1000U;

	return 0;
}

static int sc2720_fgu_sample_to_microamp(const struct sc2720_fgu *fgu,
					 const struct sc2720_fgu_sample *sample,
					 int *microamp)
{
	s32 delta;
	s64 microamps;
	s64 scaled_delta;

	if (sample->chip_id_low != SC2720_EXPECTED_ID_LOW ||
	    sample->chip_id_high != SC2720_EXPECTED_ID_HIGH)
		return -ENODEV;
	if (!(sample->module_en0 & SC2720_MODULE_EN0_FGU))
		return -EIO;
	if (!(sample->rtc_clk_en0 & SC2720_RTC_CLK_EN0_FGU) ||
	    sample->soft_rst0 & SC2720_SOFT_RST0_FGU ||
	    sample->chgr_det_fgu_ctrl & SC2720_CHGR_DET_FGU_ANALOG_MASK)
		return -ENODATA;
	if (sample->config & (SC2720_FGU_CONFIG_DISABLE |
			      SC2720_FGU_CONFIG_SW_DIS_CURT) ||
	    sample->adc_config & (SC2720_FGU_ADC_CONFIG_RESET |
				  SC2720_FGU_ADC_CONFIG_POWER_DOWN |
				  SC2720_FGU_ADC_CONFIG_SOFTWARE_FORCE_MASK) ||
	    !(sample->status & SC2720_FGU_STATUS_TOP_SELECTED) ||
	    !(sample->int_raw & SC2720_FGU_INT_RAW_CURRENT_VALID) ||
	    sample->current_raw & SC2720_FGU_CURRENT_RESERVED ||
	    !fgu->codes_per_1000ma)
		return -ENODATA;

	delta = (s32)(sample->current_raw & SC2720_FGU_CURRENT_COUNTS_MASK) -
		SC2720_FGU_CURRENT_ZERO;
	if (check_mul_overflow((s64)delta, 1000000LL, &scaled_delta))
		return -ERANGE;
	if (scaled_delta < 0)
		microamps = -(s64)DIV_ROUND_CLOSEST_ULL((u64)-scaled_delta,
							fgu->codes_per_1000ma);
	else
		microamps = DIV_ROUND_CLOSEST_ULL(scaled_delta,
						  fgu->codes_per_1000ma);
	if (microamps < INT_MIN || microamps > INT_MAX)
		return -ERANGE;

	*microamp = (int)microamps;
	return 0;
}

static int
sc2720_fgu_charge_sample_to_uah(const struct sc2720_fgu *fgu,
				const struct sc2720_fgu_charge_sample *sample,
				int *microamp_hours)
{
	u64 denominator;
	u32 raw_count;
	u32 extension;
	s32 count;
	s64 charge;
	s64 scaled_count;
	int current_microamps;
	int ret;

	ret = sc2720_fgu_sample_to_microamp(fgu, &sample->state,
					    &current_microamps);
	if (ret)
		return ret;

	raw_count = ((u32)sample->high_before << 16) | sample->low;
	extension = raw_count & SC2720_FGU_CLBCNT_SIGN_EXTENSION;
	if (extension && extension != SC2720_FGU_CLBCNT_SIGN_EXTENSION)
		return -ENODATA;

	count = sign_extend32(raw_count, SC2720_FGU_CLBCNT_SIGN_BIT);
	denominator = (u64)fgu->codes_per_1000ma * SC2720_FGU_CLBCNT_SAMPLE_HZ *
		      SC2720_FGU_SECONDS_PER_HOUR;
	if (!denominator)
		return -ENODATA;

	/* The accumulator adds one calibrated current code every 500 ms. */
	scaled_count = (s64)count * SC2720_FGU_MICROAMPS_PER_AMP;
	if (scaled_count < 0)
		charge = -(s64)DIV_ROUND_CLOSEST_ULL((u64)-scaled_count,
						     denominator);
	else
		charge = DIV_ROUND_CLOSEST_ULL((u64)scaled_count, denominator);
	if (charge < INT_MIN || charge > INT_MAX)
		return -ERANGE;

	*microamp_hours = (int)charge;
	return 0;
}

static int sc2720_fgu_get_charge_counter(const struct sc2720_fgu *fgu,
					 int *microamp_hours)
{
	struct sc2720_fgu_charge_sample sample = {};
	int ret;

	ret = sc2720_fgu_read_charge_sample(&sample);
	if (ret)
		return ret;

	return sc2720_fgu_charge_sample_to_uah(fgu, &sample, microamp_hours);
}

static int sc2720_fgu_wait_usable(const struct sc2720_fgu *fgu)
{
	struct sc2720_fgu_sample sample = {};
	unsigned int waited;
	int microvolt;
	int ret;

	for (waited = 0; waited <= SC2720_FGU_READY_TIMEOUT_MS;
	     waited += SC2720_FGU_READY_POLL_MS) {
		ret = sc2720_fgu_read_sample(&sample);
		if (ret)
			return ret;
		ret = sc2720_fgu_sample_to_microvolt(fgu, &sample, &microvolt);
		if (!ret)
			return 0;
		if (ret != -ENODATA)
			return ret;
		if (waited != SC2720_FGU_READY_TIMEOUT_MS)
			msleep(SC2720_FGU_READY_POLL_MS);
	}

	return -ETIMEDOUT;
}

static int sc2720_fgu_get_property(struct power_supply *supply,
				   enum power_supply_property property,
				   union power_supply_propval *value)
{
	struct sc2720_fgu *fgu = power_supply_get_drvdata(supply);
	struct sc2720_fgu_sample sample = {};
	int ret;

	if (property == POWER_SUPPLY_PROP_CHARGE_COUNTER)
		return sc2720_fgu_get_charge_counter(fgu, &value->intval);

	ret = sc2720_fgu_read_sample(&sample);
	if (ret)
		return ret;

	switch (property) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return sc2720_fgu_sample_to_microvolt(fgu, &sample,
						      &value->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return sc2720_fgu_sample_to_microamp(fgu, &sample,
						     &value->intval);
	default:
		return -EINVAL;
	}
}

static enum power_supply_property sc2720_fgu_properties[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
};

static const struct power_supply_desc sc2720_fgu_description = {
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sc2720_fgu_properties,
	.num_properties = ARRAY_SIZE(sc2720_fgu_properties),
	.get_property = sc2720_fgu_get_property,
};

static int sc2720_fgu_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	struct power_supply *supply;
	struct sc2720_fgu *fgu;
	u32 current_calibration[2];
	bool pclk_attempted;
	bool pclk_cleared;
	int cleanup_ret;
	u16 efuse3;
	int ret;

	fgu = devm_kzalloc(&pdev->dev, sizeof(*fgu), GFP_KERNEL);
	if (!fgu)
		return -ENOMEM;
	fgu->dev = &pdev->dev;
	fgu->description = sc2720_fgu_description;
	ret = device_property_read_string(&pdev->dev, "label",
					  &fgu->description.name);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "missing supply label\n");
	ret = device_property_read_u32_array(&pdev->dev,
					     "fplinux,current-calibration",
					     current_calibration,
					     ARRAY_SIZE(current_calibration));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "missing current calibration\n");
	if (!current_calibration[0] || !current_calibration[1])
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"current calibration values must be nonzero\n");

	ret = sc2720_fgu_read_efuse3(&pdev->dev, &efuse3);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 efuse calibration unavailable\n");
	ret = sc2720_fgu_calibrate(fgu, efuse3, current_calibration[0],
				   current_calibration[1]);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 efuse calibration invalid\n");

	ret = sc2720_fgu_enable_pclk(&pclk_attempted);
	if (ret) {
		if (pclk_attempted) {
			cleanup_ret = sc2720_fgu_clear_pclk_best_effort(
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
	ret = devm_add_action_or_reset(&pdev->dev, sc2720_fgu_release_pclk,
				       fgu);
	if (ret)
		return ret;
	ret = sc2720_fgu_wait_usable(fgu);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 voltage sample unavailable\n");

	config.drv_data = fgu;
	config.fwnode = dev_fwnode(&pdev->dev);
	supply = devm_power_supply_register(&pdev->dev, &fgu->description,
					    &config);
	return PTR_ERR_OR_ZERO(supply);
}

static const struct of_device_id sc2720_fgu_of_match[] = {
	{ .compatible = "sprd,ums9117-sc2720-fgu" },
	{},
};
MODULE_DEVICE_TABLE(of, sc2720_fgu_of_match);

static struct platform_driver sc2720_fgu_driver = {
	.probe = sc2720_fgu_probe,
	.driver = {
		.name = "sc2720-fgu-ums9117",
		.of_match_table = sc2720_fgu_of_match,
	},
};
module_platform_driver(sc2720_fgu_driver);

MODULE_DESCRIPTION("Read-only SC2720 battery telemetry on UMS9117");
MODULE_LICENSE("GPL");
