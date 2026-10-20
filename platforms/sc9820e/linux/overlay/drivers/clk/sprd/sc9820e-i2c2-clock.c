// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "sc9820e-gate.h"

#define SC9820E_I2C2_SOURCE_FIELDS (GENMASK(1, 0) | GENMASK(10, 8))
#define SC9820E_AP_APB_ENABLE 0x0
#define SC9820E_AP_APB_I2C2 BIT(10)

struct sc9820e_i2c2_clock {
	struct clk_hw source;
	struct sc9820e_gate gate;
	void __iomem *base;
};

static unsigned long sc9820e_i2c2_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct sc9820e_i2c2_clock *clock =
		container_of(hw, struct sc9820e_i2c2_clock, source);

	/* An inherited PLL parent or divider is outside this provider's scope. */
	if (readl(clock->base) & SC9820E_I2C2_SOURCE_FIELDS)
		return 0;
	return parent_rate;
}

static const struct clk_ops sc9820e_i2c2_source_ops = {
	.recalc_rate = sc9820e_i2c2_recalc_rate,
};

static int sc9820e_i2c2_clock_probe(struct platform_device *pdev)
{
	const struct clk_parent_data parent = { .fw_name = "source" };
	struct clk_init_data source_init = {
		.name = "sc9820e-i2c2-source",
		.ops = &sc9820e_i2c2_source_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	struct clk_init_data gate_init = {
		.name = "sc9820e-i2c2-enable",
		.ops = &sc9820e_gate_ops,
	};
	struct device *dev = &pdev->dev;
	struct sc9820e_i2c2_clock *clock;
	struct clk_hw_onecell_data *clocks;
	struct resource *resource;
	int ret;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource_size(resource) != sizeof(u32))
		return dev_err_probe(dev, -EINVAL,
				     "expected one I2C2 clock register\n");

	clock = devm_kzalloc(dev, sizeof(*clock), GFP_KERNEL);
	if (!clock)
		return -ENOMEM;
	clocks = devm_kzalloc(dev, struct_size(clocks, hws, 2), GFP_KERNEL);
	if (!clocks)
		return -ENOMEM;

	clock->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(clock->base))
		return PTR_ERR(clock->base);
	clock->gate.regmap = syscon_regmap_lookup_by_phandle(
		dev->of_node, "fplinux,ap-apb-syscon");
	if (IS_ERR(clock->gate.regmap))
		return dev_err_probe(dev, PTR_ERR(clock->gate.regmap),
				     "failed to get AP-APB register map\n");

	clock->source.init = &source_init;
	ret = devm_clk_hw_register(dev, &clock->source);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register I2C2 source clock\n");
	clock->gate.dev = dev;
	clock->gate.name = "sc9820e-i2c2";
	clock->gate.offset = SC9820E_AP_APB_ENABLE;
	clock->gate.mask = SC9820E_AP_APB_I2C2;
	clock->gate.hw.init = &gate_init;
	ret = devm_clk_hw_register(dev, &clock->gate.hw);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register I2C2 module gate\n");

	clocks->num = 2;
	clocks->hws[0] = &clock->source;
	clocks->hws[1] = &clock->gate.hw;
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clocks);
}

static const struct of_device_id sc9820e_i2c2_clock_match[] = {
	{ .compatible = "fplinux,sc9820e-i2c2-clock" },
	{}
};
MODULE_DEVICE_TABLE(of, sc9820e_i2c2_clock_match);

static struct platform_driver sc9820e_i2c2_clock_driver = {
	.probe = sc9820e_i2c2_clock_probe,
	.driver = {
		.name = "sc9820e-i2c2-clock",
		.of_match_table = sc9820e_i2c2_clock_match,
	},
};
module_platform_driver(sc9820e_i2c2_clock_driver);

MODULE_DESCRIPTION("SC9820E I2C2 inherited oscillator clock and module gate");
MODULE_LICENSE("GPL");
