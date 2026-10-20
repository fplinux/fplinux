// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/fplinux,sc9820e-sdio-clock.h>

#include "sc9820e-gate.h"

#define SC9820E_AP_AHB_EB 0x0
#define SC9820E_AP_AHB_EB_SDIO0 BIT(8)
#define SC9820E_SDIO_MUX_SHIFT 0
#define SC9820E_SDIO_MUX_WIDTH 3

static int sc9820e_sdio_clock_probe(struct platform_device *pdev)
{
	static const struct clk_parent_data parents[] = {
		{ .fw_name = "ext1m" },
		{ .fw_name = "ext26m" },
	};
	struct clk_init_data gate_init = {
		.name = "sc9820e-sdio0-ahb",
		.ops = &sc9820e_gate_ops,
	};
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *clocks;
	struct sc9820e_gate *gate;
	struct resource *resource;
	struct clk_hw *mux;
	void __iomem *base;
	unsigned long mux_flags = CLK_SET_RATE_NO_REPARENT |
				  CLK_GET_RATE_NOCACHE;
	int ret;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource_size(resource) != sizeof(u32))
		return dev_err_probe(dev, -EINVAL,
				     "expected one SDIO0 clock register\n");

	gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return -ENOMEM;

	clocks = devm_kzalloc(dev,
			      struct_size(clocks, hws, SC9820E_SDIO0_CLK_COUNT),
			      GFP_KERNEL);
	if (!clocks)
		return -ENOMEM;
	clocks->num = SC9820E_SDIO0_CLK_COUNT;

	gate->regmap = syscon_regmap_lookup_by_phandle(dev->of_node,
						       "fplinux,ap-ahb-syscon");
	if (IS_ERR(gate->regmap))
		return dev_err_probe(dev, PTR_ERR(gate->regmap),
				     "failed to get AP-AHB register map\n");
	gate->dev = dev;
	gate->name = "SDIO0 AHB";
	gate->offset = SC9820E_AP_AHB_EB;
	gate->mask = SC9820E_AP_AHB_EB_SDIO0;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	mux = devm_clk_hw_register_mux_parent_data_table(
		dev, "sc9820e-sdio0-mux", parents, ARRAY_SIZE(parents),
		mux_flags, base, SC9820E_SDIO_MUX_SHIFT, SC9820E_SDIO_MUX_WIDTH,
		0, NULL, NULL);
	if (IS_ERR(mux))
		return dev_err_probe(dev, PTR_ERR(mux),
				     "failed to register SDIO0 clock mux\n");
	clocks->hws[SC9820E_SDIO0_MUX] = mux;

	gate->hw.init = &gate_init;
	ret = devm_clk_hw_register(dev, &gate->hw);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register SDIO0 AHB gate\n");
	clocks->hws[SC9820E_SDIO0_AHB] = &gate->hw;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clocks);
}

static const struct of_device_id sc9820e_sdio_clock_match[] = {
	{ .compatible = "fplinux,sc9820e-sdio-clock" },
	{}
};
MODULE_DEVICE_TABLE(of, sc9820e_sdio_clock_match);

static struct platform_driver sc9820e_sdio_clock_driver = {
	.probe = sc9820e_sdio_clock_probe,
	.driver = {
		.name = "sc9820e-sdio-clock",
		.of_match_table = sc9820e_sdio_clock_match,
	},
};
module_platform_driver(sc9820e_sdio_clock_driver);

MODULE_DESCRIPTION("SC9820E SDIO0 oscillator mux and AP-AHB gate");
MODULE_LICENSE("GPL");
