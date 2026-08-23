// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/iio/iio.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/sprd/ums9117-adi.h>

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_MODULE_EN0 0xc08U
#define SC2720_ARM_CLK_EN0 0xc0cU
#define SC2720_SOFT_RST0 0xc14U
#define SC2720_XTL_WAIT_CTRL0 0xde8U

#define SC2720_AUXADC_VERSION 0x400U
#define SC2720_AUXADC_CTRL 0x404U
#define SC2720_AUXADC_SW_CH_CFG 0x408U
#define SC2720_AUXADC_DATA 0x450U
#define SC2720_AUXADC_INT_EN 0x454U
#define SC2720_AUXADC_INT_CLR 0x458U
#define SC2720_AUXADC_INT_STS 0x45cU
#define SC2720_AUXADC_INT_RAW 0x460U
#define SC2720_AUXADC_DEBUG 0x464U
#define SC2720_AUXADC_FAST_HW_TIMER_EN 0x468U

#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U
#define SC2720_AUXADC_EXPECTED_VERSION 0x0600U

#define SC2720_MODULE_EN0_ADC BIT(5)
#define SC2720_ARM_CLK_EN0_AUXADC BIT(5)
#define SC2720_ARM_CLK_EN0_AUXAD BIT(6)
#define SC2720_SOFT_RST0_ADC BIT(6)
#define SC2720_XTL_WAIT_CTRL0_EN BIT(8)

#define SC2720_AUXADC_CTRL_EN BIT(0)
#define SC2720_AUXADC_CTRL_RUN BIT(1)
#define SC2720_AUXADC_CTRL_RUN_NUM_MASK GENMASK(7, 4)
#define SC2720_AUXADC_CTRL_AVERAGE_MASK GENMASK(10, 8)
#define SC2720_AUXADC_CTRL_OFFSET_CAL BIT(12)
#define SC2720_AUXADC_CTRL_OWNED_MASK                                        \
	(SC2720_AUXADC_CTRL_EN | SC2720_AUXADC_CTRL_RUN |                    \
	 SC2720_AUXADC_CTRL_RUN_NUM_MASK | SC2720_AUXADC_CTRL_AVERAGE_MASK | \
	 SC2720_AUXADC_CTRL_OFFSET_CAL)

#define SC2720_AUXADC_SW_CH_CFG_MASK 0x065fU
#define SC2720_AUXADC_INT BIT(0)
#define SC2720_AUXADC_DATA_MASK GENMASK(11, 0)
#define SC2720_AUXADC_DEBUG_STATE_MASK GENMASK(10, 8)
#define SC2720_AUXADC_DEBUG_COUNTER_MASK GENMASK(7, 0)
#define SC2720_AUXADC_FAST_HW_TIMER_MASK GENMASK(7, 0)

#define SC2720_AUXADC_SETTLE_MIN_US 100U
#define SC2720_AUXADC_SETTLE_MAX_US 200U
#define SC2720_AUXADC_POLL_MIN_US 100U
#define SC2720_AUXADC_POLL_MAX_US 200U
#define SC2720_AUXADC_TIMEOUT_US 10000U

struct ta1618_auxadc {
	struct device *dev;
	struct mutex lock;
	bool faulted;
};

struct ta1618_auxadc_gates {
	bool module_enabled;
	bool auxadc_clock_enabled;
	bool auxad_clock_enabled;
	bool xtl_enabled;
};

struct ta1618_auxadc_bank {
	u16 ctrl;
	u16 sw_ch_cfg;
	u16 int_en;
	bool valid;
};

struct ta1618_auxadc_session {
	struct ta1618_auxadc_gates gates;
	struct ta1618_auxadc_bank bank;
	bool gates_active;
};

static int
ta1618_auxadc_finish_transaction(struct ums9117_adi_transaction *transaction,
				 int ret, bool *uncertain)
{
	int end_ret;

	end_ret = ums9117_adi_end(transaction);
	if (end_ret || ums9117_adi_is_poisoned())
		*uncertain = true;
	if (ret)
		return ret;
	if (end_ret)
		return end_ret;
	if (ums9117_adi_is_poisoned())
		return -EIO;
	return 0;
}

static int ta1618_auxadc_read_if_ok(struct ums9117_adi_transaction *transaction,
				    int ret, u32 offset, u16 *value)
{
	if (ret)
		return ret;

	return ums9117_adi_read(transaction, offset, value);
}

static bool ta1618_auxadc_debug_idle(u16 debug)
{
	return !(debug & (SC2720_AUXADC_DEBUG_STATE_MASK |
			  SC2720_AUXADC_DEBUG_COUNTER_MASK));
}

static int ta1618_auxadc_validate_identity(u16 id_low, u16 id_high)
{
	if (id_low != SC2720_EXPECTED_ID_LOW ||
	    id_high != SC2720_EXPECTED_ID_HIGH)
		return -ENODEV;

	return 0;
}

static int ta1618_auxadc_check_identity(void)
{
	struct ums9117_adi_transaction transaction = {};
	bool uncertain = false;
	u16 id_low;
	u16 id_high;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, SC2720_CHIP_ID_LOW, &id_low);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_CHIP_ID_HIGH,
				       &id_high);
	ret = ta1618_auxadc_finish_transaction(&transaction, ret, &uncertain);
	if (ret)
		return ret;

	return ta1618_auxadc_validate_identity(id_low, id_high);
}

static int ta1618_auxadc_enable_gates(struct ta1618_auxadc_session *session,
				      bool *uncertain)
{
	struct ums9117_adi_transaction transaction = {};
	u16 arm_clk_en0;
	u16 id_high;
	u16 id_low;
	u16 module_en0;
	u16 soft_rst0;
	u16 xtl_wait_ctrl0;
	int ret;

	*uncertain = false;
	if (ums9117_adi_is_poisoned()) {
		*uncertain = true;
		return -EIO;
	}

	ret = ums9117_adi_begin(&transaction);
	if (ret) {
		*uncertain = ums9117_adi_is_poisoned();
		return ret;
	}

	ret = ums9117_adi_read(&transaction, SC2720_CHIP_ID_LOW, &id_low);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_CHIP_ID_HIGH,
				       &id_high);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_MODULE_EN0,
				       &module_en0);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_ARM_CLK_EN0,
				       &arm_clk_en0);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_SOFT_RST0,
				       &soft_rst0);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_XTL_WAIT_CTRL0,
				       &xtl_wait_ctrl0);
	if (!ret)
		ret = ta1618_auxadc_validate_identity(id_low, id_high);
	if (!ret && (module_en0 & SC2720_MODULE_EN0_ADC))
		ret = -EBUSY;
	if (!ret && (soft_rst0 & SC2720_SOFT_RST0_ADC))
		ret = -EHOSTDOWN;
	if (ret)
		return ta1618_auxadc_finish_transaction(&transaction, ret,
							uncertain);

	session->gates.module_enabled = true;
	session->gates_active = true;
	ret = ums9117_adi_update_bits(&transaction, SC2720_MODULE_EN0,
				      SC2720_MODULE_EN0_ADC,
				      SC2720_MODULE_EN0_ADC);
	if (!ret && !(arm_clk_en0 & SC2720_ARM_CLK_EN0_AUXAD)) {
		session->gates.auxad_clock_enabled = true;
		ret = ums9117_adi_update_bits(&transaction, SC2720_ARM_CLK_EN0,
					      SC2720_ARM_CLK_EN0_AUXAD,
					      SC2720_ARM_CLK_EN0_AUXAD);
	}
	if (!ret && !(arm_clk_en0 & SC2720_ARM_CLK_EN0_AUXADC)) {
		session->gates.auxadc_clock_enabled = true;
		ret = ums9117_adi_update_bits(&transaction, SC2720_ARM_CLK_EN0,
					      SC2720_ARM_CLK_EN0_AUXADC,
					      SC2720_ARM_CLK_EN0_AUXADC);
	}
	if (!ret && !(xtl_wait_ctrl0 & SC2720_XTL_WAIT_CTRL0_EN)) {
		session->gates.xtl_enabled = true;
		ret = ums9117_adi_update_bits(&transaction,
					      SC2720_XTL_WAIT_CTRL0,
					      SC2720_XTL_WAIT_CTRL0_EN,
					      SC2720_XTL_WAIT_CTRL0_EN);
	}
	if (ret)
		*uncertain = true;

	return ta1618_auxadc_finish_transaction(&transaction, ret, uncertain);
}

static int ta1618_auxadc_snapshot_bank(struct ta1618_auxadc_session *session,
				       bool *uncertain)
{
	struct ums9117_adi_transaction transaction = {};
	struct ta1618_auxadc_bank *bank = &session->bank;
	u16 debug;
	u16 fast_hw_timer_en;
	u16 int_raw;
	u16 int_sts;
	u16 version;
	int ret;

	*uncertain = false;
	ret = ums9117_adi_begin(&transaction);
	if (ret) {
		*uncertain = ums9117_adi_is_poisoned();
		return ret;
	}

	ret = ums9117_adi_read(&transaction, SC2720_AUXADC_VERSION, &version);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_CTRL,
				       &bank->ctrl);
	ret = ta1618_auxadc_read_if_ok(
		&transaction, ret, SC2720_AUXADC_SW_CH_CFG, &bank->sw_ch_cfg);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_INT_EN,
				       &bank->int_en);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_INT_STS,
				       &int_sts);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_INT_RAW,
				       &int_raw);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_DEBUG,
				       &debug);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret,
				       SC2720_AUXADC_FAST_HW_TIMER_EN,
				       &fast_hw_timer_en);
	ret = ta1618_auxadc_finish_transaction(&transaction, ret, uncertain);
	if (ret) {
		*uncertain = true;
		return ret;
	}

	bank->valid = true;
	if (version != SC2720_AUXADC_EXPECTED_VERSION ||
	    (bank->ctrl & SC2720_AUXADC_CTRL_RUN) ||
	    (bank->int_en & SC2720_AUXADC_INT) ||
	    (int_sts & SC2720_AUXADC_INT) || (int_raw & SC2720_AUXADC_INT) ||
	    (fast_hw_timer_en & SC2720_AUXADC_FAST_HW_TIMER_MASK) ||
	    !ta1618_auxadc_debug_idle(debug)) {
		*uncertain = true;
		return -EPROTO;
	}

	return 0;
}

static int ta1618_auxadc_clear_raw(bool *uncertain)
{
	struct ums9117_adi_transaction transaction = {};
	u16 ctrl;
	u16 debug;
	u16 int_raw;
	u16 int_sts;
	int ret;

	*uncertain = false;
	ret = ums9117_adi_begin(&transaction);
	if (ret) {
		*uncertain = ums9117_adi_is_poisoned();
		return ret;
	}
	ret = ums9117_adi_write_final(&transaction, SC2720_AUXADC_INT_CLR,
				      SC2720_AUXADC_INT);
	if (ret)
		*uncertain = true;
	ret = ta1618_auxadc_finish_transaction(&transaction, ret, uncertain);
	if (ret)
		return ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret) {
		*uncertain = true;
		return ret;
	}
	ret = ums9117_adi_read(&transaction, SC2720_AUXADC_CTRL, &ctrl);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_INT_STS,
				       &int_sts);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_INT_RAW,
				       &int_raw);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_DEBUG,
				       &debug);
	ret = ta1618_auxadc_finish_transaction(&transaction, ret, uncertain);
	if (ret) {
		*uncertain = true;
		return ret;
	}

	if ((ctrl & SC2720_AUXADC_CTRL_RUN) || (int_sts & SC2720_AUXADC_INT) ||
	    (int_raw & SC2720_AUXADC_INT) || !ta1618_auxadc_debug_idle(debug)) {
		*uncertain = true;
		return -EPROTO;
	}

	return 0;
}

static int ta1618_auxadc_start_conversion(struct ta1618_auxadc_session *session,
					  u8 channel, bool *uncertain)
{
	struct ums9117_adi_transaction transaction = {};
	u16 ctrl;
	u16 sw_ch_cfg;
	int ret;

	ret = ta1618_auxadc_clear_raw(uncertain);
	if (ret)
		return ret;

	ctrl = (session->bank.ctrl & ~SC2720_AUXADC_CTRL_OWNED_MASK) |
	       SC2720_AUXADC_CTRL_EN | SC2720_AUXADC_CTRL_OFFSET_CAL;
	sw_ch_cfg = (session->bank.sw_ch_cfg & ~SC2720_AUXADC_SW_CH_CFG_MASK) |
		    channel;

	*uncertain = false;
	ret = ums9117_adi_begin(&transaction);
	if (ret) {
		*uncertain = ums9117_adi_is_poisoned();
		return ret;
	}
	ret = ums9117_adi_write(&transaction, SC2720_AUXADC_CTRL, ctrl);
	if (!ret)
		ret = ums9117_adi_write(&transaction, SC2720_AUXADC_SW_CH_CFG,
					sw_ch_cfg);
	if (ret)
		*uncertain = true;
	ret = ta1618_auxadc_finish_transaction(&transaction, ret, uncertain);
	if (ret)
		return ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret) {
		*uncertain = ums9117_adi_is_poisoned();
		return ret;
	}
	ret = ums9117_adi_write_final(&transaction, SC2720_AUXADC_CTRL,
				      ctrl | SC2720_AUXADC_CTRL_RUN);
	if (ret)
		*uncertain = true;
	return ta1618_auxadc_finish_transaction(&transaction, ret, uncertain);
}

static int ta1618_auxadc_read_conversion(u16 *raw, bool *uncertain)
{
	ktime_t deadline;

	*uncertain = false;
	deadline = ktime_add_us(ktime_get(), SC2720_AUXADC_TIMEOUT_US);
	for (;;) {
		struct ums9117_adi_transaction transaction = {};
		u16 ctrl;
		u16 debug;
		u16 int_raw;
		int ret;

		ret = ums9117_adi_begin(&transaction);
		if (ret) {
			*uncertain = ums9117_adi_is_poisoned();
			return ret;
		}
		ret = ums9117_adi_read(&transaction, SC2720_AUXADC_CTRL, &ctrl);
		ret = ta1618_auxadc_read_if_ok(&transaction, ret,
					       SC2720_AUXADC_DEBUG, &debug);
		ret = ta1618_auxadc_read_if_ok(&transaction, ret,
					       SC2720_AUXADC_INT_RAW, &int_raw);
		ret = ta1618_auxadc_finish_transaction(&transaction, ret,
						       uncertain);
		if (ret)
			return ret;

		if (!(ctrl & SC2720_AUXADC_CTRL_RUN) &&
		    ta1618_auxadc_debug_idle(debug) &&
		    (int_raw & SC2720_AUXADC_INT)) {
			usleep_range(10, 20);
			ret = ums9117_adi_begin(&transaction);
			if (ret) {
				*uncertain = ums9117_adi_is_poisoned();
				return ret;
			}
			ret = ums9117_adi_read(&transaction, SC2720_AUXADC_CTRL,
					       &ctrl);
			ret = ta1618_auxadc_read_if_ok(
				&transaction, ret, SC2720_AUXADC_DEBUG, &debug);
			ret = ta1618_auxadc_read_if_ok(&transaction, ret,
						       SC2720_AUXADC_INT_RAW,
						       &int_raw);
			if (!ret && !(ctrl & SC2720_AUXADC_CTRL_RUN) &&
			    ta1618_auxadc_debug_idle(debug) &&
			    (int_raw & SC2720_AUXADC_INT))
				ret = ums9117_adi_read(&transaction,
						       SC2720_AUXADC_DATA, raw);
			ret = ta1618_auxadc_finish_transaction(&transaction,
							       ret, uncertain);
			if (ret)
				return ret;
			if (!(ctrl & SC2720_AUXADC_CTRL_RUN) &&
			    ta1618_auxadc_debug_idle(debug) &&
			    (int_raw & SC2720_AUXADC_INT)) {
				*raw &= SC2720_AUXADC_DATA_MASK;
				return 0;
			}

			*uncertain = true;
			return -EPROTO;
		}

		if (ktime_compare(ktime_get(), deadline) >= 0)
			return -ETIMEDOUT;

		usleep_range(SC2720_AUXADC_POLL_MIN_US,
			     SC2720_AUXADC_POLL_MAX_US);
	}
}

static int ta1618_auxadc_wait_idle(bool *uncertain)
{
	ktime_t deadline;

	*uncertain = false;
	deadline = ktime_add_us(ktime_get(), SC2720_AUXADC_TIMEOUT_US);
	for (;;) {
		struct ums9117_adi_transaction transaction = {};
		u16 ctrl;
		u16 debug;
		int ret;

		ret = ums9117_adi_begin(&transaction);
		if (ret) {
			*uncertain = true;
			return ret;
		}
		ret = ums9117_adi_read(&transaction, SC2720_AUXADC_CTRL, &ctrl);
		ret = ta1618_auxadc_read_if_ok(&transaction, ret,
					       SC2720_AUXADC_DEBUG, &debug);
		ret = ta1618_auxadc_finish_transaction(&transaction, ret,
						       uncertain);
		if (ret) {
			*uncertain = true;
			return ret;
		}
		if (!(ctrl & SC2720_AUXADC_CTRL_RUN) &&
		    ta1618_auxadc_debug_idle(debug))
			return 0;
		if (ktime_compare(ktime_get(), deadline) >= 0)
			return -ETIMEDOUT;

		usleep_range(SC2720_AUXADC_POLL_MIN_US,
			     SC2720_AUXADC_POLL_MAX_US);
	}
}

static int ta1618_auxadc_restore_bank(struct ta1618_auxadc_session *session,
				      bool *uncertain)
{
	struct ums9117_adi_transaction transaction = {};
	struct ta1618_auxadc_bank *bank = &session->bank;
	u16 ctrl;
	u16 ctrl_final;
	u16 debug;
	u16 int_en;
	u16 int_raw;
	u16 int_sts;
	u16 sw_ch_cfg;
	int ret;

	ret = ta1618_auxadc_wait_idle(uncertain);
	if (ret)
		return ret;
	ret = ta1618_auxadc_clear_raw(uncertain);
	if (ret)
		return ret;

	*uncertain = false;
	ret = ums9117_adi_begin(&transaction);
	if (ret) {
		*uncertain = true;
		return ret;
	}
	ret = ums9117_adi_read(&transaction, SC2720_AUXADC_SW_CH_CFG,
			       &sw_ch_cfg);
	if (!ret) {
		sw_ch_cfg = (sw_ch_cfg & ~SC2720_AUXADC_SW_CH_CFG_MASK) |
			    (bank->sw_ch_cfg & SC2720_AUXADC_SW_CH_CFG_MASK);
		ret = ums9117_adi_write(&transaction, SC2720_AUXADC_SW_CH_CFG,
					sw_ch_cfg);
	}
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_CTRL,
				       &ctrl);
	if (!ret) {
		ctrl_final = (ctrl & ~SC2720_AUXADC_CTRL_OWNED_MASK) |
			     (bank->ctrl & SC2720_AUXADC_CTRL_OWNED_MASK);
		ret = ums9117_adi_write(&transaction, SC2720_AUXADC_CTRL,
					ctrl_final & ~SC2720_AUXADC_CTRL_EN);
		if (!ret)
			ret = ums9117_adi_write(&transaction,
						SC2720_AUXADC_CTRL, ctrl_final);
	}
	if (ret)
		*uncertain = true;
	ret = ta1618_auxadc_finish_transaction(&transaction, ret, uncertain);
	if (ret)
		return ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret) {
		*uncertain = true;
		return ret;
	}
	ret = ums9117_adi_read(&transaction, SC2720_AUXADC_CTRL, &ctrl);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret,
				       SC2720_AUXADC_SW_CH_CFG, &sw_ch_cfg);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_INT_EN,
				       &int_en);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_INT_STS,
				       &int_sts);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_INT_RAW,
				       &int_raw);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_AUXADC_DEBUG,
				       &debug);
	ret = ta1618_auxadc_finish_transaction(&transaction, ret, uncertain);
	if (ret) {
		*uncertain = true;
		return ret;
	}
	if ((ctrl & SC2720_AUXADC_CTRL_OWNED_MASK) !=
		    (bank->ctrl & SC2720_AUXADC_CTRL_OWNED_MASK) ||
	    (sw_ch_cfg & SC2720_AUXADC_SW_CH_CFG_MASK) !=
		    (bank->sw_ch_cfg & SC2720_AUXADC_SW_CH_CFG_MASK) ||
	    int_en != bank->int_en || (int_sts & SC2720_AUXADC_INT) ||
	    (int_raw & SC2720_AUXADC_INT) || !ta1618_auxadc_debug_idle(debug)) {
		*uncertain = true;
		return -EPROTO;
	}

	return 0;
}

static int ta1618_auxadc_restore_gates(struct ta1618_auxadc_session *session,
				       bool *uncertain)
{
	struct ums9117_adi_transaction transaction = {};
	struct ta1618_auxadc_gates *gates = &session->gates;
	u16 arm_clk_en0;
	u16 module_en0;
	u16 xtl_wait_ctrl0;
	int ret;

	*uncertain = false;
	ret = ums9117_adi_begin(&transaction);
	if (ret) {
		*uncertain = true;
		return ret;
	}
	if (gates->xtl_enabled)
		ret = ums9117_adi_update_bits(&transaction,
					      SC2720_XTL_WAIT_CTRL0,
					      SC2720_XTL_WAIT_CTRL0_EN, 0);
	if (!ret && gates->auxadc_clock_enabled)
		ret = ums9117_adi_update_bits(&transaction, SC2720_ARM_CLK_EN0,
					      SC2720_ARM_CLK_EN0_AUXADC, 0);
	if (!ret && gates->auxad_clock_enabled)
		ret = ums9117_adi_update_bits(&transaction, SC2720_ARM_CLK_EN0,
					      SC2720_ARM_CLK_EN0_AUXAD, 0);
	if (!ret && gates->module_enabled)
		ret = ums9117_adi_update_bits(&transaction, SC2720_MODULE_EN0,
					      SC2720_MODULE_EN0_ADC, 0);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_MODULE_EN0,
				       &module_en0);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_ARM_CLK_EN0,
				       &arm_clk_en0);
	ret = ta1618_auxadc_read_if_ok(&transaction, ret, SC2720_XTL_WAIT_CTRL0,
				       &xtl_wait_ctrl0);
	if (ret)
		*uncertain = true;
	ret = ta1618_auxadc_finish_transaction(&transaction, ret, uncertain);
	if (ret) {
		*uncertain = true;
		return ret;
	}
	if ((gates->module_enabled && (module_en0 & SC2720_MODULE_EN0_ADC)) ||
	    (gates->auxadc_clock_enabled &&
	     (arm_clk_en0 & SC2720_ARM_CLK_EN0_AUXADC)) ||
	    (gates->auxad_clock_enabled &&
	     (arm_clk_en0 & SC2720_ARM_CLK_EN0_AUXAD)) ||
	    (gates->xtl_enabled &&
	     (xtl_wait_ctrl0 & SC2720_XTL_WAIT_CTRL0_EN))) {
		*uncertain = true;
		return -EPROTO;
	}

	return 0;
}

static int ta1618_auxadc_sample(struct ta1618_auxadc *adc, u8 channel, int *raw)
{
	struct ta1618_auxadc_session session = {};
	bool uncertain = false;
	u16 sample;
	int cleanup_ret;
	int ret;

	mutex_lock(&adc->lock);
	if (adc->faulted || ums9117_adi_is_poisoned()) {
		adc->faulted = true;
		ret = -EIO;
		goto unlock;
	}

	ret = ta1618_auxadc_enable_gates(&session, &uncertain);
	if (ret)
		goto finish;
	usleep_range(SC2720_AUXADC_SETTLE_MIN_US, SC2720_AUXADC_SETTLE_MAX_US);
	ret = ta1618_auxadc_snapshot_bank(&session, &uncertain);
	if (ret)
		goto finish;
	ret = ta1618_auxadc_start_conversion(&session, channel, &uncertain);
	if (ret)
		goto finish;
	ret = ta1618_auxadc_read_conversion(&sample, &uncertain);

finish:
	if (uncertain) {
		adc->faulted = true;
		dev_err_ratelimited(
			adc->dev,
			"SC2720 AUXADC state is uncertain; reads disabled\n");
		goto unlock;
	}

	cleanup_ret = 0;
	if (session.bank.valid)
		cleanup_ret = ta1618_auxadc_restore_bank(&session, &uncertain);
	if (!cleanup_ret && session.gates_active)
		cleanup_ret = ta1618_auxadc_restore_gates(&session, &uncertain);
	if (cleanup_ret || uncertain) {
		adc->faulted = true;
		dev_err_ratelimited(
			adc->dev,
			"SC2720 AUXADC cleanup failed; reads disabled\n");
		if (!ret)
			ret = cleanup_ret ? cleanup_ret : -EIO;
		goto unlock;
	}

	if (!ret)
		*raw = sample;

unlock:
	mutex_unlock(&adc->lock);
	return ret;
}

static int ta1618_auxadc_read_raw(struct iio_dev *indio_dev,
				  struct iio_chan_spec const *chan, int *val,
				  int *val2, long mask)
{
	struct ta1618_auxadc *adc = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ta1618_auxadc_sample(adc, chan->channel, val);
		return ret ? ret : IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info ta1618_auxadc_info = {
	.read_raw = ta1618_auxadc_read_raw,
};

#define TA1618_AUXADC_CHANNEL(_channel)                       \
	{                                                     \
		.type = IIO_VOLTAGE,                          \
		.indexed = 1,                                 \
		.channel = (_channel),                        \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	}

static const struct iio_chan_spec ta1618_auxadc_channels[] = {
	TA1618_AUXADC_CHANNEL(0),  TA1618_AUXADC_CHANNEL(1),
	TA1618_AUXADC_CHANNEL(2),  TA1618_AUXADC_CHANNEL(4),
	TA1618_AUXADC_CHANNEL(14),
};

static int ta1618_auxadc_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct ta1618_auxadc *adc;
	int ret;

	ret = ta1618_auxadc_check_identity();
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 AUXADC is unavailable\n");

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->dev = &pdev->dev;
	mutex_init(&adc->lock);
	indio_dev->name = "ta1618-sc2720-auxadc";
	indio_dev->info = &ta1618_auxadc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ta1618_auxadc_channels;
	indio_dev->num_channels = ARRAY_SIZE(ta1618_auxadc_channels);

	return devm_iio_device_register(&pdev->dev, indio_dev);
}

static const struct of_device_id ta1618_auxadc_of_match[] = {
	{ .compatible = "fplinux,ta1618-sc2720-auxadc" },
	{},
};
MODULE_DEVICE_TABLE(of, ta1618_auxadc_of_match);

static struct platform_driver ta1618_auxadc_driver = {
	.probe = ta1618_auxadc_probe,
	.driver = {
		.name = "ta1618-sc2720-auxadc",
		.of_match_table = ta1618_auxadc_of_match,
	},
};
module_platform_driver(ta1618_auxadc_driver);

MODULE_DESCRIPTION("Nokia TA-1618 SC2720 direct AUXADC raw provider");
MODULE_LICENSE("GPL");
