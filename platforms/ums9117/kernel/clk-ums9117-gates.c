// SPDX-License-Identifier: GPL-2.0-only

#include <dt-bindings/clock/sprd,ums9117-clk.h>
#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "common.h"
#include "gate.h"

/*
 * UMS9117 Device Specification V1.0, sections 6.22.2.1, 6.22.2.2 and
 * 6.22.2.5 define the AON APB state registers, gate bits and SET/CLEAR
 * aliases. The generated UMS9117 register definition glb/ap_ahb.h identifies
 * AHB_EB at offset 0x0, with DMA_EB at bit 5 and SDIO0_EB at bit 7. The
 * vendor global-register access contract in chip_drv_common_io.h defines SET
 * at +0x1000 and CLEAR at +0x2000 for both banks.
 */
#define UMS9117_GATE_SC_OFFSET 0x1000
#define UMS9117_SDIO0_SELECTOR_MASK GENMASK(2, 0)
#define UMS9117_SDIO0_SELECTOR_RPLL_390M 4
#define UMS9117_SDIO0_RATE_HZ 195000000UL

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

static struct sprd_clk_common *ums9117_aonapb_gate_clks[] = {
	&adi_eb.common,	 &splk_eb.common, &efuse_eb.common,
	&mbox_eb.common, &thm1_eb.common, &thm_rtc_eb.common,
};

static struct clk_hw_onecell_data ums9117_aonapb_gate_hws = {
	.hws = {
		[CLK_ADI_EB] = &adi_eb.common.hw,
		[CLK_SPLK_EB] = &splk_eb.common.hw,
		[CLK_EFUSE_EB] = &efuse_eb.common.hw,
		[CLK_MBOX_EB] = &mbox_eb.common.hw,
		[CLK_THM1_EB] = &thm1_eb.common.hw,
		[CLK_THM_RTC_EB] = &thm_rtc_eb.common.hw,
	},
	.num = CLK_AON_APB_GATE_NUM,
};

static const struct sprd_clk_desc ums9117_aonapb_gate_desc = {
	.clk_clks = ums9117_aonapb_gate_clks,
	.num_clk_clks = ARRAY_SIZE(ums9117_aonapb_gate_clks),
	.hw_clks = &ums9117_aonapb_gate_hws,
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
			"failed to restore SDIO0 selector after prepare error: %d\n",
			restore_ret);
	return ret;
}

static void ums9117_sdio0_clk_unprepare(struct clk_hw *hw)
{
	struct ums9117_sdio0_clk *sdio = hw_to_ums9117_sdio0_clk(hw);
	int ret;

	ret = ums9117_sdio0_clk_restore_selector(sdio);
	if (ret)
		dev_err(sdio->dev, "failed to restore SDIO0 selector: %d\n",
			ret);
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
	{ .compatible = "sprd,ums9117-ap-clk", .data = &ums9117_ap_clk_desc },
	{ .compatible = "sprd,ums9117-aonapb-gate",
	  .data = &ums9117_aonapb_gate_desc },
	{ .compatible = "sprd,ums9117-apahb-gate",
	  .data = &ums9117_apahb_gate_desc },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_clk_ids);

static int ums9117_preserve_boot_enabled_gates(struct device *dev,
					       const struct sprd_clk_desc *desc)
{
	struct clk_init_data *critical_inits;
	struct sprd_clk_common *common;
	const struct clk_init_data *init;
	struct clk_hw *hw;
	unsigned long boot_enabled_mask = 0;
	unsigned long critical_count = 0;
	unsigned long critical_index = 0;
	unsigned long index;
	int enabled;

	if (desc->num_clk_clks > BITS_PER_LONG)
		return -E2BIG;
	for (index = 0; index < desc->num_clk_clks; index++) {
		common = desc->clk_clks[index];
		if (!common)
			continue;
		hw = &common->hw;
		init = hw->init;
		if (!init || init->ops != &sprd_sc_gate_ops ||
		    (init->flags & CLK_IS_CRITICAL))
			continue;

		/*
		 * clk_hw_is_enabled() requires the core created by registration.
		 * The SC-gate callback only needs the regmap assigned above.
		 */
		enabled = init->ops->is_enabled(hw);
		if (enabled < 0)
			return enabled;
		if (enabled) {
			boot_enabled_mask |= BIT(index);
			critical_count++;
		}
	}
	if (!critical_count)
		return 0;

	critical_inits = devm_kcalloc(dev, critical_count,
				      sizeof(*critical_inits), GFP_KERNEL);
	if (!critical_inits)
		return -ENOMEM;

	for (index = 0; index < desc->num_clk_clks; index++) {
		if (!(boot_enabled_mask & BIT(index)))
			continue;
		common = desc->clk_clks[index];
		hw = &common->hw;
		init = hw->init;

		critical_inits[critical_index] = *init;
		critical_inits[critical_index].flags |= CLK_IS_CRITICAL;
		hw->init = &critical_inits[critical_index];
		critical_index++;
	}

	return 0;
}

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
	if (desc == &ums9117_ap_clk_desc)
		sdio0_clk.dev = &pdev->dev;
	ret = ums9117_preserve_boot_enabled_gates(&pdev->dev, desc);
	if (ret)
		return ret;

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
