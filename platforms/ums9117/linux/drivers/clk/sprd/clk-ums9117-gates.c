// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <dt-bindings/clock/sprd,ums9117-clk.h>
#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/errno.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "common.h"
#include "composite.h"
#include "gate.h"

/* Gate updates use write-one SET/CLEAR aliases, not shared-state RMW. */
#define UMS9117_GATE_SC_OFFSET 0x1000
#define UMS9117_SDIO0_SELECTOR_MASK GENMASK(2, 0)
#define UMS9117_SDIO0_SELECTOR_RPLL_390M 4
#define UMS9117_SDIO0_RATE_HZ 195000000UL
#define UMS9117_SENSOR_SOURCE_RATE_HZ 48000000UL
#define UMS9117_SENSOR_CFG_MASK (GENMASK(9, 8) | GENMASK(1, 0))
#define UMS9117_SENSOR_SOURCE_48M 1
#define UMS9117_SENSOR_GATE_REG 0x5fc
#define UMS9117_SENSOR_GATE BIT(0)
#define UMS9117_SENSOR_RATE_IDENTIFY_HZ 12000000UL
#define UMS9117_SENSOR_RATE_CAPTURE_HZ 24000000UL

/*
 * Selector 4 chooses the vendor RPLL_390M output for CGM_SDIO0_2X. The
 * controller consumes its fixed divide-by-two output as the 195 MHz base
 * clock. The source PLL is inherited and is not controlled by this provider.
 * Prepare owns only the three selector bits and restores their prior value
 * after the consumer has stopped using the clock.
 */
struct ums9117_sdio0_clk {
	struct sprd_clk_common common;
	struct device *dev;
	u32 selector_snapshot;
	bool selector_owned;
};

/* The 48 MHz input is inherited; this clock does not control its source. */
struct ums9117_sensor_clk {
	struct sprd_clk_common common;
	struct regmap *gate_regmap;
	struct device *dev;
	u32 cfg_snapshot;
	unsigned long requested_rate;
	bool gate_snapshot;
	bool owned;
};

static SPRD_SC_GATE_CLK_NO_PARENT(adi_eb, "adi-eb", 0x0, UMS9117_GATE_SC_OFFSET,
				  BIT(16), CLK_IS_CRITICAL, 0);
static SPRD_SC_GATE_CLK_NO_PARENT(splk_eb, "splk-eb", 0x0,
				  UMS9117_GATE_SC_OFFSET, BIT(22),
				  CLK_IS_CRITICAL, 0);
static SPRD_SC_GATE_CLK_NO_PARENT(efuse_eb, "efuse-eb", 0x0,
				  UMS9117_GATE_SC_OFFSET, BIT(13), 0, 0);
static SPRD_SC_GATE_CLK_NO_PARENT(mbox_eb, "mbox-eb", 0x4,
				  UMS9117_GATE_SC_OFFSET, BIT(21),
				  CLK_IS_CRITICAL, 0);
static SPRD_SC_GATE_CLK_NO_PARENT(thm1_eb, "thm1-eb", 0x4,
				  UMS9117_GATE_SC_OFFSET, BIT(19), 0, 0);
static SPRD_SC_GATE_CLK_NO_PARENT(thm_rtc_eb, "thm-rtc-eb", 0x10,
				  UMS9117_GATE_SC_OFFSET, BIT(10), 0, 0);
static SPRD_SC_GATE_CLK_NO_PARENT(gpio_eb, "gpio-eb", 0x0,
				  UMS9117_GATE_SC_OFFSET, BIT(3), 0, 0);
/* These clocks also serve the coprocessor EIC banks. */
static SPRD_SC_GATE_CLK_NO_PARENT(eic_eb, "eic-eb", 0x0, UMS9117_GATE_SC_OFFSET,
				  BIT(14), CLK_IS_CRITICAL, 0);
static SPRD_SC_GATE_CLK_NO_PARENT(eic_rtc_eb, "eic-rtc-eb", 0x10,
				  UMS9117_GATE_SC_OFFSET, BIT(6),
				  CLK_IS_CRITICAL, 0);
static SPRD_SC_GATE_CLK_NO_PARENT(eic_rtcdv5_eb, "eic-rtcdv5-eb", 0x10,
				  UMS9117_GATE_SC_OFFSET, BIT(7),
				  CLK_IS_CRITICAL, 0);

static struct sprd_clk_common *ums9117_aonapb_gate_clks[] = {
	&adi_eb.common,	       &splk_eb.common, &efuse_eb.common,
	&mbox_eb.common,       &thm1_eb.common, &thm_rtc_eb.common,
	&gpio_eb.common,       &eic_eb.common,	&eic_rtc_eb.common,
	&eic_rtcdv5_eb.common,
};

static struct clk_hw_onecell_data ums9117_aonapb_gate_hws = {
	.hws = {
		[CLK_ADI_EB] = &adi_eb.common.hw,
		[CLK_SPLK_EB] = &splk_eb.common.hw,
		[CLK_EFUSE_EB] = &efuse_eb.common.hw,
		[CLK_MBOX_EB] = &mbox_eb.common.hw,
		[CLK_THM1_EB] = &thm1_eb.common.hw,
		[CLK_THM_RTC_EB] = &thm_rtc_eb.common.hw,
		[CLK_GPIO_EB] = &gpio_eb.common.hw,
		[CLK_EIC_EB] = &eic_eb.common.hw,
		[CLK_EIC_RTC_EB] = &eic_rtc_eb.common.hw,
		[CLK_EIC_RTCDV5_EB] = &eic_rtcdv5_eb.common.hw,
	},
	.num = CLK_AON_APB_GATE_NUM,
};

static const struct sprd_clk_desc ums9117_aonapb_gate_desc = {
	.clk_clks = ums9117_aonapb_gate_clks,
	.num_clk_clks = ARRAY_SIZE(ums9117_aonapb_gate_clks),
	.hw_clks = &ums9117_aonapb_gate_hws,
};

static SPRD_SC_GATE_CLK_NO_PARENT(i2c0_eb, "i2c0-eb", 0x0,
				  UMS9117_GATE_SC_OFFSET, BIT(8), 0, 0);

static struct sprd_clk_common *ums9117_apapb_gate_clks[] = {
	&i2c0_eb.common,
};

static struct clk_hw_onecell_data ums9117_apapb_gate_hws = {
	.hws = {
		[CLK_I2C0_EB] = &i2c0_eb.common.hw,
	},
	.num = CLK_AP_APB_GATE_NUM,
};

static const struct sprd_clk_desc ums9117_apapb_gate_desc = {
	.clk_clks = ums9117_apapb_gate_clks,
	.num_clk_clks = ARRAY_SIZE(ums9117_apapb_gate_clks),
	.hw_clks = &ums9117_apapb_gate_hws,
};

static struct ums9117_sdio0_clk *hw_to_ums9117_sdio0_clk(struct clk_hw *hw)
{
	struct sprd_clk_common *common = hw_to_sprd_clk_common(hw);

	return container_of(common, struct ums9117_sdio0_clk, common);
}

static int ums9117_sdio0_clk_write_selector(struct ums9117_sdio0_clk *sdio,
					    u32 selector)
{
	unsigned int value;
	int ret;

	ret = regmap_update_bits(sdio->common.regmap, sdio->common.reg,
				 UMS9117_SDIO0_SELECTOR_MASK, selector);
	if (ret)
		return ret;
	ret = regmap_read(sdio->common.regmap, sdio->common.reg, &value);
	if (ret)
		return ret;

	return (value & UMS9117_SDIO0_SELECTOR_MASK) == selector ? 0 : -EIO;
}

static int ums9117_sdio0_clk_restore_selector(struct ums9117_sdio0_clk *sdio)
{
	int ret;

	if (!sdio->selector_owned)
		return 0;
	ret = ums9117_sdio0_clk_write_selector(sdio, sdio->selector_snapshot);
	sdio->selector_owned = false;

	return ret;
}

static int ums9117_sdio0_clk_prepare(struct clk_hw *hw)
{
	struct ums9117_sdio0_clk *sdio = hw_to_ums9117_sdio0_clk(hw);
	unsigned int value;
	int restore_ret;
	int ret;

	ret = regmap_read(sdio->common.regmap, sdio->common.reg, &value);
	if (ret)
		return ret;
	sdio->selector_snapshot = value & UMS9117_SDIO0_SELECTOR_MASK;
	sdio->selector_owned = true;

	/* Switch through selector 0 before selecting RPLL. */
	ret = ums9117_sdio0_clk_write_selector(sdio, 0);
	if (!ret)
		ret = ums9117_sdio0_clk_write_selector(
			sdio, UMS9117_SDIO0_SELECTOR_RPLL_390M);
	if (!ret)
		return 0;

	restore_ret = ums9117_sdio0_clk_restore_selector(sdio);
	if (restore_ret)
		dev_err(sdio->dev,
			"failed to restore SDIO0 selector after prepare error: %pe\n",
			ERR_PTR(restore_ret));
	return ret;
}

static void ums9117_sdio0_clk_unprepare(struct clk_hw *hw)
{
	struct ums9117_sdio0_clk *sdio = hw_to_ums9117_sdio0_clk(hw);
	int ret;

	ret = ums9117_sdio0_clk_restore_selector(sdio);
	if (ret)
		dev_err(sdio->dev, "failed to restore SDIO0 selector: %pe\n",
			ERR_PTR(ret));
}

static unsigned long ums9117_sdio0_clk_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct sprd_clk_common *common = hw_to_sprd_clk_common(hw);
	unsigned int selector;

	if (regmap_read(common->regmap, common->reg, &selector))
		return 0;
	if ((selector & UMS9117_SDIO0_SELECTOR_MASK) !=
	    UMS9117_SDIO0_SELECTOR_RPLL_390M)
		return 0;

	return UMS9117_SDIO0_RATE_HZ;
}

static const struct clk_ops ums9117_sdio0_clk_ops = {
	.prepare = ums9117_sdio0_clk_prepare,
	.unprepare = ums9117_sdio0_clk_unprepare,
	.recalc_rate = ums9117_sdio0_clk_recalc_rate,
};

static struct ums9117_sdio0_clk sdio0_clk = {
	.common = {
		.regmap = NULL,
		.reg = 0,
		.hw.init = CLK_HW_INIT_NO_PARENT("sdio",
					      &ums9117_sdio0_clk_ops,
					      CLK_GET_RATE_NOCACHE),
	},
};

static struct ums9117_sensor_clk *hw_to_ums9117_sensor_clk(struct clk_hw *hw)
{
	struct sprd_clk_common *common = hw_to_sprd_clk_common(hw);

	return container_of(common, struct ums9117_sensor_clk, common);
}

static unsigned long ums9117_sensor_cfg_rate(u32 cfg, unsigned long parent_rate)
{
	if ((cfg & GENMASK(1, 0)) != UMS9117_SENSOR_SOURCE_48M)
		return 0;

	return parent_rate / (((cfg >> 8) & 3) + 1);
}

static int ums9117_sensor_write_cfg(struct ums9117_sensor_clk *sensor, u32 cfg)
{
	unsigned int value;
	int ret;

	ret = regmap_update_bits(sensor->common.regmap, sensor->common.reg,
				 UMS9117_SENSOR_CFG_MASK, cfg);
	if (ret)
		return ret;
	ret = regmap_read(sensor->common.regmap, sensor->common.reg, &value);
	if (ret)
		return ret;

	return (value & UMS9117_SENSOR_CFG_MASK) == cfg ? 0 : -EIO;
}

static int ums9117_sensor_write_gate(struct ums9117_sensor_clk *sensor,
				     bool enable)
{
	unsigned int value;
	int ret;

	ret = regmap_write(sensor->gate_regmap,
			   UMS9117_SENSOR_GATE_REG +
				   (enable ? UMS9117_GATE_SC_OFFSET :
					     2 * UMS9117_GATE_SC_OFFSET),
			   UMS9117_SENSOR_GATE);
	if (ret)
		return ret;
	ret = regmap_read(sensor->gate_regmap, UMS9117_SENSOR_GATE_REG, &value);
	if (ret)
		return ret;

	return !!(value & UMS9117_SENSOR_GATE) == enable ? 0 : -EIO;
}

static int ums9117_sensor_restore(struct ums9117_sensor_clk *sensor)
{
	int ret;
	int step_ret;

	ret = ums9117_sensor_write_gate(sensor, false);
	step_ret = ums9117_sensor_write_cfg(sensor, sensor->cfg_snapshot);
	if (!ret)
		ret = step_ret;
	step_ret = ums9117_sensor_write_gate(sensor, sensor->gate_snapshot);
	if (!ret)
		ret = step_ret;
	sensor->owned = false;

	return ret;
}

static int ums9117_sensor_prepare(struct clk_hw *hw)
{
	struct ums9117_sensor_clk *sensor = hw_to_ums9117_sensor_clk(hw);
	struct clk_hw *parent = clk_hw_get_parent(hw);
	unsigned int cfg;
	unsigned int gate;
	unsigned long rate;
	u32 selected_cfg;
	int ret;
	int restore_ret;

	if (!parent || clk_hw_get_rate(parent) != UMS9117_SENSOR_SOURCE_RATE_HZ)
		return -EINVAL;
	ret = regmap_read(sensor->common.regmap, sensor->common.reg, &cfg);
	if (ret)
		return ret;
	ret = regmap_read(sensor->gate_regmap, UMS9117_SENSOR_GATE_REG, &gate);
	if (ret)
		return ret;

	rate = sensor->requested_rate;
	if (!rate)
		rate = ums9117_sensor_cfg_rate(cfg,
					       UMS9117_SENSOR_SOURCE_RATE_HZ);
	if (rate != UMS9117_SENSOR_RATE_IDENTIFY_HZ &&
	    rate != UMS9117_SENSOR_RATE_CAPTURE_HZ)
		return -EINVAL;

	sensor->cfg_snapshot = cfg & UMS9117_SENSOR_CFG_MASK;
	sensor->gate_snapshot = !!(gate & UMS9117_SENSOR_GATE);
	sensor->owned = true;
	selected_cfg = UMS9117_SENSOR_SOURCE_48M |
		       ((UMS9117_SENSOR_SOURCE_RATE_HZ / rate - 1) << 8);

	ret = ums9117_sensor_write_gate(sensor, false);
	if (!ret)
		ret = ums9117_sensor_write_cfg(sensor, selected_cfg);
	if (!ret)
		ret = ums9117_sensor_write_gate(sensor, true);
	if (!ret)
		return 0;

	restore_ret = ums9117_sensor_restore(sensor);
	if (restore_ret)
		dev_err(sensor->dev,
			"failed to restore sensor clock after prepare error: %pe\n",
			ERR_PTR(restore_ret));
	return ret;
}

static void ums9117_sensor_unprepare(struct clk_hw *hw)
{
	struct ums9117_sensor_clk *sensor = hw_to_ums9117_sensor_clk(hw);
	int ret;

	if (!sensor->owned)
		return;
	ret = ums9117_sensor_restore(sensor);
	if (ret)
		dev_err(sensor->dev, "failed to restore sensor clock: %pe\n",
			ERR_PTR(ret));
}

static unsigned long ums9117_sensor_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct ums9117_sensor_clk *sensor = hw_to_ums9117_sensor_clk(hw);
	unsigned int cfg;

	if (sensor->requested_rate)
		return sensor->requested_rate;
	if (regmap_read(sensor->common.regmap, sensor->common.reg, &cfg))
		return 0;

	return ums9117_sensor_cfg_rate(cfg, parent_rate);
}

static long ums9117_sensor_round_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long *parent_rate)
{
	if (*parent_rate != UMS9117_SENSOR_SOURCE_RATE_HZ ||
	    (rate != UMS9117_SENSOR_RATE_IDENTIFY_HZ &&
	     rate != UMS9117_SENSOR_RATE_CAPTURE_HZ))
		return -EINVAL;

	return rate;
}

static int ums9117_sensor_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct ums9117_sensor_clk *sensor = hw_to_ums9117_sensor_clk(hw);

	if (parent_rate != UMS9117_SENSOR_SOURCE_RATE_HZ ||
	    (rate != UMS9117_SENSOR_RATE_IDENTIFY_HZ &&
	     rate != UMS9117_SENSOR_RATE_CAPTURE_HZ))
		return -EINVAL;

	/* The register is changed only after prepare has saved its prior value. */
	sensor->requested_rate = rate;
	return 0;
}

static const struct clk_ops ums9117_sensor_clk_ops = {
	.prepare = ums9117_sensor_prepare,
	.unprepare = ums9117_sensor_unprepare,
	.recalc_rate = ums9117_sensor_recalc_rate,
	.round_rate = ums9117_sensor_round_rate,
	.set_rate = ums9117_sensor_set_rate,
};

static struct ums9117_sensor_clk sensor_clk = {
	.common = {
		.reg = 0,
		.hw.init = CLK_HW_INIT_FW_NAME("sensor0", "source",
					    &ums9117_sensor_clk_ops,
					    CLK_SET_RATE_GATE | CLK_GET_RATE_NOCACHE),
	},
};

static struct sprd_clk_common *ums9117_aon_sensor_clk_clks[] = {
	&sensor_clk.common,
};

static struct clk_hw_onecell_data ums9117_aon_sensor_clk_hws = {
	.hws = {
		[CLK_SENSOR0] = &sensor_clk.common.hw,
	},
	.num = CLK_AON_SENSOR_CLK_NUM,
};

static const struct sprd_clk_desc ums9117_aon_sensor_clk_desc = {
	.clk_clks = ums9117_aon_sensor_clk_clks,
	.num_clk_clks = ARRAY_SIZE(ums9117_aon_sensor_clk_clks),
	.hw_clks = &ums9117_aon_sensor_clk_hws,
};

static const char *const i2c0_parents[] = { "ext-26m" };

static SPRD_COMP_CLK(i2c0_clk, "i2c0", i2c0_parents, 0x0, 0, 2, 8, 3, 0);

static struct sprd_clk_common *ums9117_ap_i2c0_clk_clks[] = {
	&i2c0_clk.common,
};

static struct clk_hw_onecell_data ums9117_ap_i2c0_clk_hws = {
	.hws = {
		[CLK_AP_I2C0] = &i2c0_clk.common.hw,
	},
	.num = CLK_AP_I2C0_CLK_NUM,
};

static const struct sprd_clk_desc ums9117_ap_i2c0_clk_desc = {
	.clk_clks = ums9117_ap_i2c0_clk_clks,
	.num_clk_clks = ARRAY_SIZE(ums9117_ap_i2c0_clk_clks),
	.hw_clks = &ums9117_ap_i2c0_clk_hws,
};

static struct sprd_clk_common *ums9117_ap_clk_clks[] = {
	&sdio0_clk.common,
};

static struct clk_hw_onecell_data ums9117_ap_clk_hws = {
	.hws = {
		[CLK_SDIO0] = &sdio0_clk.common.hw,
	},
	.num = CLK_AP_CLK_NUM,
};

static const struct sprd_clk_desc ums9117_ap_clk_desc = {
	.clk_clks = ums9117_ap_clk_clks,
	.num_clk_clks = ARRAY_SIZE(ums9117_ap_clk_clks),
	.hw_clks = &ums9117_ap_clk_hws,
};

static SPRD_SC_GATE_CLK_NO_PARENT(dma_eb, "dma-eb", 0x0, UMS9117_GATE_SC_OFFSET,
				  BIT(5), 0, 0);
static SPRD_SC_GATE_CLK_NO_PARENT(sdio0_eb, "sdio0-eb", 0x0,
				  UMS9117_GATE_SC_OFFSET, BIT(7), 0, 0);

static struct sprd_clk_common *ums9117_apahb_gate_clks[] = {
	&dma_eb.common,
	&sdio0_eb.common,
};

static struct clk_hw_onecell_data ums9117_apahb_gate_hws = {
	.hws = {
		[CLK_DMA_EB] = &dma_eb.common.hw,
		[CLK_SDIO0_EB] = &sdio0_eb.common.hw,
	},
	.num = CLK_AP_AHB_GATE_NUM,
};

static const struct sprd_clk_desc ums9117_apahb_gate_desc = {
	.clk_clks = ums9117_apahb_gate_clks,
	.num_clk_clks = ARRAY_SIZE(ums9117_apahb_gate_clks),
	.hw_clks = &ums9117_apahb_gate_hws,
};

static const struct of_device_id ums9117_clk_ids[] = {
	{ .compatible = "sprd,ums9117-aon-sensor-clk",
	  .data = &ums9117_aon_sensor_clk_desc },
	{ .compatible = "sprd,ums9117-ap-clk", .data = &ums9117_ap_clk_desc },
	{ .compatible = "sprd,ums9117-ap-i2c0-clk",
	  .data = &ums9117_ap_i2c0_clk_desc },
	{ .compatible = "sprd,ums9117-aonapb-gate",
	  .data = &ums9117_aonapb_gate_desc },
	{ .compatible = "sprd,ums9117-apapb-gate",
	  .data = &ums9117_apapb_gate_desc },
	{ .compatible = "sprd,ums9117-apahb-gate",
	  .data = &ums9117_apahb_gate_desc },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_clk_ids);

static int ums9117_clk_probe(struct platform_device *pdev)
{
	const struct sprd_clk_desc *desc;
	int ret;

	desc = device_get_match_data(&pdev->dev);
	if (!desc)
		return -ENODEV;

	ret = sprd_clk_regmap_init(pdev, desc);
	if (ret)
		return ret;
	if (desc == &ums9117_aon_sensor_clk_desc) {
		sensor_clk.gate_regmap = syscon_regmap_lookup_by_phandle(
			pdev->dev.of_node, "sprd,aon-apb");
		if (IS_ERR(sensor_clk.gate_regmap))
			return dev_err_probe(
				&pdev->dev, PTR_ERR(sensor_clk.gate_regmap),
				"failed to get sensor gate regmap\n");
		sensor_clk.dev = &pdev->dev;
	}
	if (desc == &ums9117_ap_i2c0_clk_desc) {
		/* Only the 26 MHz source with divider 1 is used for I2C0. */
		ret = regmap_update_bits(i2c0_clk.common.regmap, 0,
					 GENMASK(10, 8) | GENMASK(1, 0), 0);
		if (ret)
			return ret;
	}
	if (desc == &ums9117_ap_clk_desc)
		sdio0_clk.dev = &pdev->dev;

	return sprd_clk_probe(&pdev->dev, desc->hw_clks);
}

static struct platform_driver ums9117_clk_driver = {
	.probe = ums9117_clk_probe,
	.driver = {
		.name = "ums9117-clk",
		.of_match_table = ums9117_clk_ids,
	},
};
module_platform_driver(ums9117_clk_driver);

MODULE_DESCRIPTION("Unisoc UMS9117 clock driver");
MODULE_LICENSE("GPL v2");
