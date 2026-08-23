// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/thermal.h>

#define TA1618_THM_PHYS 0x40300000U
#define TA1618_THM_BYTES 0x100U
#define TA1618_EFUSE_CONTROL_PHYS 0x40240000U
#define TA1618_EFUSE_CONTROL_BYTES 0x58U
#define TA1618_EFUSE_BLOCK25_PHYS 0x40241064U
#define TA1618_EFUSE_BLOCK25_BYTES 0x4U

#define TA1618_AON_APB_EB0 0x000U
#define TA1618_AON_APB_EB1 0x004U
#define TA1618_AON_APB_RST0 0x008U
#define TA1618_AON_APB_RTC_EB 0x010U
#define TA1618_AON_APB_EB0_SET 0x1000U
#define TA1618_AON_APB_EB0_CLEAR 0x2000U
#define TA1618_AON_APB_EFUSE_EB BIT(13)
#define TA1618_AON_APB_EFUSE_RST BIT(14)
#define TA1618_AON_APB_THM1_EB BIT(19)
#define TA1618_AON_APB_THM_RTC_EB BIT(10)

#define TA1618_EFUSE_IP_VER 0x014U
#define TA1618_EFUSE_NS_EN 0x020U
#define TA1618_EFUSE_NS_ERR 0x024U
#define TA1618_EFUSE_NS_MAGIC 0x02cU
#define TA1618_EFUSE_PW_SWT 0x054U

#define TA1618_EFUSE_IP_VERSION 0x0800U
#define TA1618_EFUSE_IP_TYPE_MASK GENMASK(17, 16)
#define TA1618_EFUSE_NS_ERR_MASK GENMASK(13, 0)
#define TA1618_EFUSE_NS_ENABLE_VALUE 0x5U
#define TA1618_EFUSE_PW_SWT_READ GENMASK(2, 0)
#define TA1618_EFUSE_PW_SWT_READ_VALUE BIT(1)
#define TA1618_EFUSE_CALIBRATION_MASK GENMASK(30, 16)
#define TA1618_EFUSE_SETTLE_MIN_US 2000U
#define TA1618_EFUSE_SETTLE_MAX_US 2500U

#define TA1618_THM_CTL 0x000U
#define TA1618_THM_INT_EN 0x004U
#define TA1618_THM_MON_CTRL 0x050U
#define TA1618_THM_INTERNAL_STS1 0x054U
#define TA1618_THM_SENSOR0 0x05cU
#define TA1618_THM_VERSION 0x0fcU

#define TA1618_THM_EXPECTED_VERSION 0x00000400U
#define TA1618_THM_CTL_ENABLE BIT(0)
#define TA1618_THM_CTL_MON_ENABLE BIT(1)
#define TA1618_THM_CTL_SENSOR_MASK GENMASK(9, 2)
#define TA1618_THM_CTL_SENSOR0 BIT(2)
#define TA1618_THM_CTL_UNNORMAL_INT_MODE BIT(10)
#define TA1618_THM_CTL_SOFT_RESET BIT(11)
#define TA1618_THM_CTL_SET_READY BIT(12)
#define TA1618_THM_CTL_SET_READY_ST BIT(13)
#define TA1618_THM_CTL_RESET_ST GENMASK(15, 14)
#define TA1618_THM_CTL_WRITABLE GENMASK(11, 0)
#define TA1618_THM_CTL_ACTIVE                                \
	(TA1618_THM_CTL_ENABLE | TA1618_THM_CTL_MON_ENABLE | \
	 TA1618_THM_CTL_SENSOR0)
#define TA1618_THM_CTL_GUARD_MASK                                        \
	(TA1618_THM_CTL_ENABLE | TA1618_THM_CTL_MON_ENABLE |             \
	 TA1618_THM_CTL_SENSOR_MASK | TA1618_THM_CTL_UNNORMAL_INT_MODE | \
	 TA1618_THM_CTL_SOFT_RESET | TA1618_THM_CTL_SET_READY |          \
	 TA1618_THM_CTL_SET_READY_ST | TA1618_THM_CTL_RESET_ST)
#define TA1618_THM_MON_CONFIG_MASK GENMASK(3, 0)
#define TA1618_THM_MON_CONFIG_VALUE 0x7U
#define TA1618_THM_STS1_READY BIT(0)
#define TA1618_THM_SENSOR_RAW_MASK GENMASK(9, 0)
#define TA1618_THM_SET_READY_TIMEOUT_US 10000U
#define TA1618_THM_SET_READY_POLL_DELAY_US 10U
#define TA1618_THM_READY_TIMEOUT_US 2000000U
#define TA1618_THM_READY_POLL_DELAY_US 1000U

struct ta1618_thm1 {
	struct device *dev;
	void __iomem *thm;
	void __iomem *efuse_control;
	void __iomem *efuse_block25;
	struct regmap *aon_apb;
	struct thermal_zone_device *zone;
	u32 ratio_permille;
	s64 delta_mc;
	u32 aon_eb1_initial;
	u32 aon_rtc_eb_initial;
	u32 ctl_initial;
	u32 mon_initial;
	bool calibration_valid;
	bool efuse_gate_initially_on;
	bool efuse_gate_enabled_confirmed;
	bool efuse_gate_snapshot_valid;
	bool efuse_gate_set_attempted;
	bool efuse_ns_enable_attempted;
	bool efuse_ns_disable_attempted;
	bool efuse_ns_known_off;
	bool efuse_cleanup_complete;
	int efuse_cleanup_error;
	bool thm_gates_snapshot_valid;
	bool thm_eb1_write_attempted;
	bool thm_rtc_eb_write_attempted;
	bool thm_ctl_write_attempted;
	bool thm_mon_write_attempted;
};

static atomic_t ta1618_thm1_efuse_attempted = ATOMIC_INIT(0);

static const struct thermal_zone_params ta1618_thm1_zone_params = {
	.no_hwmon = true,
};

static int ta1618_thm1_read_aon_gates(struct ta1618_thm1 *thm, u32 *eb1,
				      u32 *rtc_eb)
{
	int ret;

	ret = regmap_read(thm->aon_apb, TA1618_AON_APB_EB1, eb1);
	if (ret)
		return ret;

	return regmap_read(thm->aon_apb, TA1618_AON_APB_RTC_EB, rtc_eb);
}

static int ta1618_thm1_update_gate(struct ta1618_thm1 *thm, unsigned int offset,
				   u32 mask, u32 value, bool *write_attempted)
{
	u32 readback;
	int ret;

	*write_attempted = true;
	ret = regmap_update_bits(thm->aon_apb, offset, mask, value);
	if (ret)
		return ret;
	ret = regmap_read(thm->aon_apb, offset, &readback);
	if (ret)
		return ret;

	return (readback & mask) == (value & mask) ? 0 : -EIO;
}

static int ta1618_thm1_restore_gate(struct ta1618_thm1 *thm,
				    unsigned int offset, u32 mask, u32 initial,
				    bool write_attempted)
{
	if (!write_attempted)
		return 0;

	return ta1618_thm1_update_gate(thm, offset, mask, initial,
				       &write_attempted);
}

static int ta1618_thm1_restore_gates(struct ta1618_thm1 *thm)
{
	int eb1_ret;
	int rtc_ret;

	if (!thm->thm_gates_snapshot_valid)
		return 0;

	rtc_ret = ta1618_thm1_restore_gate(thm, TA1618_AON_APB_RTC_EB,
					   TA1618_AON_APB_THM_RTC_EB,
					   thm->aon_rtc_eb_initial,
					   thm->thm_rtc_eb_write_attempted);
	eb1_ret = ta1618_thm1_restore_gate(thm, TA1618_AON_APB_EB1,
					   TA1618_AON_APB_THM1_EB,
					   thm->aon_eb1_initial,
					   thm->thm_eb1_write_attempted);

	return rtc_ret ? rtc_ret : eb1_ret;
}

static int ta1618_thm1_write_ctl(struct ta1618_thm1 *thm, u32 writable)
{
	u32 ctl;

	ctl = readl(thm->thm + TA1618_THM_CTL);
	thm->thm_ctl_write_attempted = true;
	writel((ctl & ~TA1618_THM_CTL_WRITABLE) |
		       (writable & TA1618_THM_CTL_WRITABLE),
	       thm->thm + TA1618_THM_CTL);
	ctl = readl(thm->thm + TA1618_THM_CTL);

	return (ctl & TA1618_THM_CTL_WRITABLE) ==
			       (writable & TA1618_THM_CTL_WRITABLE) ?
		       0 :
		       -EIO;
}

static int ta1618_thm1_disable(struct ta1618_thm1 *thm)
{
	u32 ctl;

	ctl = readl(thm->thm + TA1618_THM_CTL);
	thm->thm_ctl_write_attempted = true;
	writel(ctl & ~TA1618_THM_CTL_ENABLE, thm->thm + TA1618_THM_CTL);
	ctl = readl(thm->thm + TA1618_THM_CTL);

	return ctl & TA1618_THM_CTL_ENABLE ? -EIO : 0;
}

static void ta1618_thm1_issue_set_ready(struct ta1618_thm1 *thm)
{
	u32 ctl;

	ctl = readl(thm->thm + TA1618_THM_CTL);
	thm->thm_ctl_write_attempted = true;
	writel(ctl | TA1618_THM_CTL_SET_READY, thm->thm + TA1618_THM_CTL);
}

static int ta1618_thm1_wait_set_ready(struct ta1618_thm1 *thm)
{
	u32 ctl;

	return readl_poll_timeout(thm->thm + TA1618_THM_CTL, ctl,
				  !(ctl & TA1618_THM_CTL_SET_READY_ST),
				  TA1618_THM_SET_READY_POLL_DELAY_US,
				  TA1618_THM_SET_READY_TIMEOUT_US);
}

static int ta1618_thm1_restore_state(struct ta1618_thm1 *thm)
{
	u32 ctl;
	u32 mon;
	int ret;

	ret = ta1618_thm1_disable(thm);
	if (ret)
		return ret;
	if (thm->thm_mon_write_attempted) {
		writel(thm->mon_initial, thm->thm + TA1618_THM_MON_CTRL);
		mon = readl(thm->thm + TA1618_THM_MON_CTRL);
		if ((mon & TA1618_THM_MON_CONFIG_MASK) !=
		    (thm->mon_initial & TA1618_THM_MON_CONFIG_MASK))
			return -EIO;
	}
	ret = ta1618_thm1_write_ctl(thm,
				    thm->ctl_initial & TA1618_THM_CTL_WRITABLE);
	if (ret)
		return ret;
	ta1618_thm1_issue_set_ready(thm);
	ret = ta1618_thm1_wait_set_ready(thm);
	if (ret)
		return ret;
	ctl = readl(thm->thm + TA1618_THM_CTL);

	return (ctl & TA1618_THM_CTL_WRITABLE) ==
			       (thm->ctl_initial & TA1618_THM_CTL_WRITABLE) ?
		       0 :
		       -EIO;
}

static void ta1618_thm1_record_efuse_cleanup_error(struct ta1618_thm1 *thm,
						   int error)
{
	if (error && !thm->efuse_cleanup_error)
		thm->efuse_cleanup_error = error;
}

static int ta1618_thm1_snapshot_efuse_gate(struct ta1618_thm1 *thm)
{
	unsigned int gate;
	unsigned int reset;
	int ret;

	ret = regmap_read(thm->aon_apb, TA1618_AON_APB_RST0, &reset);
	if (ret)
		return ret;
	if (reset & TA1618_AON_APB_EFUSE_RST)
		return -EBUSY;
	ret = regmap_read(thm->aon_apb, TA1618_AON_APB_EB0, &gate);
	if (ret)
		return ret;

	thm->efuse_gate_initially_on = !!(gate & TA1618_AON_APB_EFUSE_EB);
	thm->efuse_gate_enabled_confirmed = thm->efuse_gate_initially_on;
	thm->efuse_gate_snapshot_valid = true;
	return 0;
}

static int ta1618_thm1_enable_efuse_gate(struct ta1618_thm1 *thm)
{
	unsigned int gate;
	int ret;

	if (thm->efuse_gate_initially_on)
		return 0;

	thm->efuse_gate_set_attempted = true;
	ret = regmap_write(thm->aon_apb, TA1618_AON_APB_EB0_SET,
			   TA1618_AON_APB_EFUSE_EB);
	if (ret)
		return ret;
	/* Complete the gate transition before verifying it. */
	wmb();
	ret = regmap_read(thm->aon_apb, TA1618_AON_APB_EB0, &gate);
	if (ret)
		return ret;
	if (!(gate & TA1618_AON_APB_EFUSE_EB))
		return -EIO;

	thm->efuse_gate_enabled_confirmed = true;
	return 0;
}

static int ta1618_thm1_validate_efuse_controller(struct ta1618_thm1 *thm)
{
	u32 ip_ver;
	u32 ns_en;
	u32 ns_err;
	u32 ns_magic;
	u32 pw_swt;

	ip_ver = readl(thm->efuse_control + TA1618_EFUSE_IP_VER);
	if ((ip_ver & GENMASK(15, 0)) != TA1618_EFUSE_IP_VERSION ||
	    FIELD_GET(TA1618_EFUSE_IP_TYPE_MASK, ip_ver))
		return -ENODEV;
	ns_en = readl(thm->efuse_control + TA1618_EFUSE_NS_EN);
	if (ns_en)
		return -EBUSY;
	thm->efuse_ns_known_off = true;
	ns_err = readl(thm->efuse_control + TA1618_EFUSE_NS_ERR);
	if (ns_err & TA1618_EFUSE_NS_ERR_MASK)
		return -EUCLEAN;
	ns_magic = readl(thm->efuse_control + TA1618_EFUSE_NS_MAGIC);
	if (ns_magic)
		return -EBUSY;
	pw_swt = readl(thm->efuse_control + TA1618_EFUSE_PW_SWT);
	if ((pw_swt & TA1618_EFUSE_PW_SWT_READ) !=
	    TA1618_EFUSE_PW_SWT_READ_VALUE)
		return -EBUSY;

	return 0;
}

static int ta1618_thm1_enable_efuse_ns_read(struct ta1618_thm1 *thm)
{
	u32 ns_en;

	thm->efuse_ns_enable_attempted = true;
	thm->efuse_ns_known_off = false;
	writel(TA1618_EFUSE_NS_ENABLE_VALUE,
	       thm->efuse_control + TA1618_EFUSE_NS_EN);
	/* Complete the eFuse read-enable transition before verifying it. */
	wmb();
	ns_en = readl(thm->efuse_control + TA1618_EFUSE_NS_EN);
	if (ns_en != TA1618_EFUSE_NS_ENABLE_VALUE)
		return -EIO;
	usleep_range(TA1618_EFUSE_SETTLE_MIN_US, TA1618_EFUSE_SETTLE_MAX_US);

	return 0;
}

static int ta1618_thm1_disable_efuse_ns_once(struct ta1618_thm1 *thm)
{
	u32 ns_en;

	if (thm->efuse_ns_known_off)
		return 0;
	if (thm->efuse_ns_disable_attempted) {
		ta1618_thm1_record_efuse_cleanup_error(thm, -EIO);
		return thm->efuse_cleanup_error;
	}

	thm->efuse_ns_disable_attempted = true;
	writel(0, thm->efuse_control + TA1618_EFUSE_NS_EN);
	/* Flush and verify the disable before its required settling interval. */
	wmb();
	ns_en = readl(thm->efuse_control + TA1618_EFUSE_NS_EN);
	if (ns_en) {
		ta1618_thm1_record_efuse_cleanup_error(thm, -EIO);
		return thm->efuse_cleanup_error;
	}
	usleep_range(TA1618_EFUSE_SETTLE_MIN_US, TA1618_EFUSE_SETTLE_MAX_US);
	ns_en = readl(thm->efuse_control + TA1618_EFUSE_NS_EN);
	if (ns_en) {
		ta1618_thm1_record_efuse_cleanup_error(thm, -EIO);
		return thm->efuse_cleanup_error;
	}

	thm->efuse_ns_known_off = true;
	return 0;
}

static int ta1618_thm1_clear_efuse_gate(struct ta1618_thm1 *thm)
{
	unsigned int gate;
	int read_ret;
	int write_ret;

	write_ret = regmap_write(thm->aon_apb, TA1618_AON_APB_EB0_CLEAR,
				 TA1618_AON_APB_EFUSE_EB);
	ta1618_thm1_record_efuse_cleanup_error(thm, write_ret);
	/* Complete the gate-clear transition before verifying it. */
	wmb();
	read_ret = regmap_read(thm->aon_apb, TA1618_AON_APB_EB0, &gate);
	ta1618_thm1_record_efuse_cleanup_error(thm, read_ret);
	if (read_ret)
		return thm->efuse_cleanup_error;
	if (gate & TA1618_AON_APB_EFUSE_EB) {
		ta1618_thm1_record_efuse_cleanup_error(thm, -EIO);
		return thm->efuse_cleanup_error;
	}

	thm->efuse_cleanup_complete = true;
	return thm->efuse_cleanup_error;
}

static int ta1618_thm1_cleanup_efuse(struct ta1618_thm1 *thm)
{
	int ret;

	if (thm->efuse_cleanup_complete)
		return thm->efuse_cleanup_error;
	if (thm->efuse_ns_enable_attempted &&
	    !thm->efuse_gate_enabled_confirmed) {
		ta1618_thm1_record_efuse_cleanup_error(thm, -EUCLEAN);
		return thm->efuse_cleanup_error;
	}
	if (thm->efuse_ns_enable_attempted && !thm->efuse_ns_known_off) {
		ret = ta1618_thm1_disable_efuse_ns_once(thm);
		if (ret)
			return ret;
	}
	if (thm->efuse_gate_initially_on || !thm->efuse_gate_set_attempted) {
		if (!thm->efuse_ns_enable_attempted || thm->efuse_ns_known_off)
			thm->efuse_cleanup_complete = true;
		return thm->efuse_cleanup_error;
	}
	if (!thm->efuse_ns_enable_attempted || thm->efuse_ns_known_off)
		return ta1618_thm1_clear_efuse_gate(thm);

	ta1618_thm1_record_efuse_cleanup_error(thm, -EIO);
	return thm->efuse_cleanup_error;
}

static void ta1618_thm1_efuse_unwind(void *data)
{
	struct ta1618_thm1 *thm = data;
	int ret;

	if (!thm->efuse_gate_snapshot_valid)
		return;
	ret = ta1618_thm1_cleanup_efuse(thm);
	if (ret)
		dev_err(thm->dev, "eFuse block 25 cleanup failed: %d\n", ret);
}

static int ta1618_thm1_read_efuse25(struct ta1618_thm1 *thm, u32 *value)
{
	u32 block25;
	u32 ns_err;
	int cleanup_ret;
	int ret;

	ret = ta1618_thm1_enable_efuse_gate(thm);
	if (ret)
		goto out;
	ret = ta1618_thm1_validate_efuse_controller(thm);
	if (ret)
		goto out;
	ret = ta1618_thm1_enable_efuse_ns_read(thm);
	if (ret)
		goto out;
	ns_err = readl(thm->efuse_control + TA1618_EFUSE_NS_ERR);
	if (ns_err & TA1618_EFUSE_NS_ERR_MASK) {
		ret = -EUCLEAN;
		goto out;
	}
	block25 = readl(thm->efuse_block25);
	ns_err = readl(thm->efuse_control + TA1618_EFUSE_NS_ERR);
	if (ns_err & TA1618_EFUSE_NS_ERR_MASK) {
		ret = -EUCLEAN;
		goto out;
	}
	*value = block25;
	ret = 0;

out:
	cleanup_ret = ta1618_thm1_cleanup_efuse(thm);
	if (ret)
		return ret;

	return cleanup_ret;
}

static int ta1618_thm1_decode_calibration(struct ta1618_thm1 *thm,
					  u32 calibration)
{
	u32 d;
	u32 m;

	if (!(calibration & TA1618_EFUSE_CALIBRATION_MASK))
		return -ENODATA;

	d = FIELD_GET(GENMASK(30, 24), calibration);
	m = FIELD_GET(GENMASK(23, 17), calibration);
	thm->ratio_permille = calibration & BIT(16) ? 1000U + m : 1000U - m;
	thm->delta_mc = d >= 64U ? -((s64)(d - 64U) / 2) * 1000 :
				   ((s64)(64U - d) / 2) * 1000;
	thm->calibration_valid = true;

	return 0;
}

static int ta1618_thm1_prepare_thm(struct ta1618_thm1 *thm)
{
	u32 version;
	int ret;

	ret = ta1618_thm1_read_aon_gates(thm, &thm->aon_eb1_initial,
					 &thm->aon_rtc_eb_initial);
	if (ret)
		return ret;
	thm->thm_gates_snapshot_valid = true;
	if (!(thm->aon_eb1_initial & TA1618_AON_APB_THM1_EB)) {
		ret = ta1618_thm1_update_gate(thm, TA1618_AON_APB_EB1,
					      TA1618_AON_APB_THM1_EB,
					      TA1618_AON_APB_THM1_EB,
					      &thm->thm_eb1_write_attempted);
		if (ret)
			return ret;
	}
	if (!(thm->aon_rtc_eb_initial & TA1618_AON_APB_THM_RTC_EB)) {
		ret = ta1618_thm1_update_gate(thm, TA1618_AON_APB_RTC_EB,
					      TA1618_AON_APB_THM_RTC_EB,
					      TA1618_AON_APB_THM_RTC_EB,
					      &thm->thm_rtc_eb_write_attempted);
		if (ret)
			return ret;
	}
	version = readl(thm->thm + TA1618_THM_VERSION);
	if (version != TA1618_THM_EXPECTED_VERSION)
		return -ENODEV;
	thm->ctl_initial = readl(thm->thm + TA1618_THM_CTL);
	thm->mon_initial = readl(thm->thm + TA1618_THM_MON_CTRL);
	if (thm->ctl_initial &
	    (TA1618_THM_CTL_ENABLE | TA1618_THM_CTL_MON_ENABLE |
	     TA1618_THM_CTL_SET_READY | TA1618_THM_CTL_SET_READY_ST |
	     TA1618_THM_CTL_RESET_ST | TA1618_THM_CTL_SOFT_RESET |
	     TA1618_THM_CTL_UNNORMAL_INT_MODE))
		return -EBUSY;
	if ((thm->ctl_initial & TA1618_THM_CTL_SENSOR_MASK) !=
	    TA1618_THM_CTL_SENSOR0)
		return -EBUSY;
	if (readl(thm->thm + TA1618_THM_INT_EN))
		return -EBUSY;
	if (thm->mon_initial & TA1618_THM_MON_CONFIG_MASK)
		return -EBUSY;

	return 0;
}

static int ta1618_thm1_activate(struct ta1618_thm1 *thm)
{
	u32 mon;
	u32 status;
	int ret;

	thm->thm_mon_write_attempted = true;
	writel((thm->mon_initial & ~TA1618_THM_MON_CONFIG_MASK) |
		       TA1618_THM_MON_CONFIG_VALUE,
	       thm->thm + TA1618_THM_MON_CTRL);
	mon = readl(thm->thm + TA1618_THM_MON_CTRL);
	if ((mon & TA1618_THM_MON_CONFIG_MASK) != TA1618_THM_MON_CONFIG_VALUE)
		return -EIO;
	ret = ta1618_thm1_write_ctl(thm, TA1618_THM_CTL_SENSOR0 |
						 TA1618_THM_CTL_MON_ENABLE);
	if (ret)
		return ret;
	ta1618_thm1_issue_set_ready(thm);
	ret = ta1618_thm1_wait_set_ready(thm);
	if (ret)
		return ret;
	ret = ta1618_thm1_write_ctl(thm, TA1618_THM_CTL_ACTIVE);
	if (ret)
		return ret;

	return readl_poll_timeout(thm->thm + TA1618_THM_INTERNAL_STS1, status,
				  status & TA1618_THM_STS1_READY,
				  TA1618_THM_READY_POLL_DELAY_US,
				  TA1618_THM_READY_TIMEOUT_US);
}

static int ta1618_thm1_read_temp(struct ta1618_thm1 *thm, int *temp)
{
	u32 aon_eb1;
	u32 aon_rtc_eb;
	u32 ctl;
	u32 mon;
	u32 raw;
	s64 temperature;
	int ret;

	if (!thm->calibration_valid)
		return -ENODATA;
	ret = ta1618_thm1_read_aon_gates(thm, &aon_eb1, &aon_rtc_eb);
	if (ret)
		return ret;
	if (!(aon_eb1 & TA1618_AON_APB_THM1_EB) ||
	    !(aon_rtc_eb & TA1618_AON_APB_THM_RTC_EB))
		return -EIO;
	ctl = readl(thm->thm + TA1618_THM_CTL);
	if ((ctl & TA1618_THM_CTL_GUARD_MASK) != TA1618_THM_CTL_ACTIVE)
		return -EIO;
	if (readl(thm->thm + TA1618_THM_INT_EN))
		return -EIO;
	mon = readl(thm->thm + TA1618_THM_MON_CTRL);
	if ((mon & TA1618_THM_MON_CONFIG_MASK) != TA1618_THM_MON_CONFIG_VALUE)
		return -EIO;
	if (!(readl(thm->thm + TA1618_THM_INTERNAL_STS1) &
	      TA1618_THM_STS1_READY))
		return -EAGAIN;

	raw = readl(thm->thm + TA1618_THM_SENSOR0) & TA1618_THM_SENSOR_RAW_MASK;
	temperature = div_s64(912LL * thm->ratio_permille * raw, 1000) - 72396 +
		      thm->delta_mc;
	if (temperature < -20000 || temperature > 125000)
		return -ERANGE;
	*temp = temperature;

	return 0;
}

static int ta1618_thm1_get_temp(struct thermal_zone_device *zone, int *temp)
{
	return ta1618_thm1_read_temp(thermal_zone_device_priv(zone), temp);
}

static const struct thermal_zone_device_ops ta1618_thm1_ops = {
	.get_temp = ta1618_thm1_get_temp,
};

static void ta1618_thm1_unwind(void *data)
{
	struct ta1618_thm1 *thm = data;
	int ret;

	if (thm->zone) {
		thermal_zone_device_unregister(thm->zone);
		thm->zone = NULL;
	}
	if (thm->thm_ctl_write_attempted || thm->thm_mon_write_attempted) {
		ret = ta1618_thm1_restore_state(thm);
		if (ret) {
			dev_err(thm->dev, "THM1 state restoration failed: %d\n",
				ret);
			return;
		}
	}
	ret = ta1618_thm1_restore_gates(thm);
	if (ret)
		dev_err(thm->dev, "THM1 gate restoration failed: %d\n", ret);
}

static int ta1618_thm1_validate_resource(struct platform_device *pdev,
					 unsigned int index, const char *name,
					 resource_size_t start,
					 resource_size_t size,
					 struct resource **resource)
{
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	if (!res || !res->name || strcmp(res->name, name) ||
	    res->start != start || resource_size(res) != size)
		return -EINVAL;
	*resource = res;

	return 0;
}

static int ta1618_thm1_probe(struct platform_device *pdev)
{
	struct ta1618_thm1 *thm;
	struct resource *resource;
	u32 calibration;
	int temperature;
	int ret;

	thm = devm_kzalloc(&pdev->dev, sizeof(*thm), GFP_KERNEL);
	if (!thm)
		return -ENOMEM;
	thm->dev = &pdev->dev;

	ret = ta1618_thm1_validate_resource(pdev, 0, "thm", TA1618_THM_PHYS,
					    TA1618_THM_BYTES, &resource);
	if (ret)
		return dev_err_probe(
			&pdev->dev, ret,
			"THM1 DT resource does not match TA-1618\n");
	thm->thm = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(thm->thm))
		return PTR_ERR(thm->thm);
	ret = ta1618_thm1_validate_resource(pdev, 1, "efuse-control",
					    TA1618_EFUSE_CONTROL_PHYS,
					    TA1618_EFUSE_CONTROL_BYTES,
					    &resource);
	if (ret)
		return dev_err_probe(
			&pdev->dev, ret,
			"eFuse control DT resource does not match TA-1618\n");
	thm->efuse_control = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(thm->efuse_control))
		return PTR_ERR(thm->efuse_control);
	ret = ta1618_thm1_validate_resource(pdev, 2, "efuse-block25",
					    TA1618_EFUSE_BLOCK25_PHYS,
					    TA1618_EFUSE_BLOCK25_BYTES,
					    &resource);
	if (ret)
		return dev_err_probe(
			&pdev->dev, ret,
			"eFuse block 25 DT resource does not match TA-1618\n");
	if (platform_get_resource(pdev, IORESOURCE_MEM, 3))
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"exactly three DT resources are required\n");
	thm->efuse_block25 = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(thm->efuse_block25))
		return PTR_ERR(thm->efuse_block25);
	thm->aon_apb = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						       "sprd,aon-apb");
	if (IS_ERR(thm->aon_apb))
		return dev_err_probe(&pdev->dev, PTR_ERR(thm->aon_apb),
				     "could not resolve AON APB syscon\n");
	if (atomic_cmpxchg(&ta1618_thm1_efuse_attempted, 0, 1))
		return dev_err_probe(
			&pdev->dev, -EALREADY,
			"eFuse block 25 was already attempted this boot\n");

	ret = ta1618_thm1_snapshot_efuse_gate(thm);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "eFuse guard rejected access\n");
	ret = devm_add_action_or_reset(&pdev->dev, ta1618_thm1_efuse_unwind,
				       thm);
	if (ret)
		return ret;
	ret = ta1618_thm1_read_efuse25(thm, &calibration);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "eFuse block 25 read refused\n");
	ret = ta1618_thm1_decode_calibration(thm, calibration);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "THM1 calibration is unavailable\n");

	ret = devm_add_action_or_reset(&pdev->dev, ta1618_thm1_unwind, thm);
	if (ret)
		return ret;
	ret = ta1618_thm1_prepare_thm(thm);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "THM1 initial state is unsuitable\n");
	ret = ta1618_thm1_activate(thm);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "THM1 activation failed\n");
	ret = ta1618_thm1_read_temp(thm, &temperature);
	if (ret)
		return dev_err_probe(
			&pdev->dev, ret,
			"THM1 initial temperature is unavailable\n");

	thm->zone = thermal_tripless_zone_device_register(
		"ta1618-soc", thm, &ta1618_thm1_ops, &ta1618_thm1_zone_params);
	if (IS_ERR(thm->zone)) {
		ret = PTR_ERR(thm->zone);
		thm->zone = NULL;
		return dev_err_probe(&pdev->dev, ret,
				     "could not register THM1 thermal zone\n");
	}
	ret = thermal_zone_device_enable(thm->zone);
	if (ret) {
		thermal_zone_device_unregister(thm->zone);
		thm->zone = NULL;
		return dev_err_probe(&pdev->dev, ret,
				     "could not enable THM1 thermal zone\n");
	}
	platform_set_drvdata(pdev, thm);

	return 0;
}

static const struct of_device_id ta1618_thm1_of_match[] = {
	{ .compatible = "fplinux,ta1618-thm1" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_thm1_of_match);

static struct platform_driver ta1618_thm1_driver = {
	.probe = ta1618_thm1_probe,
	.driver = {
		.name = "ta1618-thm1",
		.of_match_table = ta1618_thm1_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ta1618_thm1_driver);

MODULE_DESCRIPTION("Nokia TA-1618 UMS9117 THM1 thermal sensor");
MODULE_LICENSE("GPL");
