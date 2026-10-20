// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include "sc9820e-gate.h"

#define SC9820E_AON_APB_EB0 0x0
#define SC9820E_AON_APB_GPIO BIT(3)
#define SC9820E_AON_APB_EFUSE BIT(13)
#define SC9820E_AON_APB_HWSPINLOCK BIT(22)
#define SC9820E_AON_APB_EB1 0x4
#define SC9820E_AON_APB_MAILBOX BIT(21)
#define SC9820E_AON_APB_HS_SPI_EB 0x134
#define SC9820E_AON_APB_HS_SPI BIT(9)
#define SC9820E_AP_AHB_EB 0x0
#define SC9820E_AP_AHB_DMA BIT(5)

struct sc9820e_peripheral_gate_data {
	const char *name;
	const char *syscon_property;
	u32 offset;
	u32 mask;
};

static int sc9820e_peripheral_gate_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct sc9820e_peripheral_gate_data *data =
		device_get_match_data(dev);
	struct clk_init_data init = {
		.name = data->name,
		.ops = &sc9820e_gate_ops,
	};
	struct sc9820e_gate *gate;
	int ret;

	gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return -ENOMEM;

	gate->regmap = syscon_regmap_lookup_by_phandle(dev->of_node,
						       data->syscon_property);
	if (IS_ERR(gate->regmap))
		return dev_err_probe(dev, PTR_ERR(gate->regmap),
				     "failed to get %s register map\n",
				     data->name);

	gate->dev = dev;
	gate->name = data->name;
	gate->offset = data->offset;
	gate->mask = data->mask;
	gate->hw.init = &init;
	ret = devm_clk_hw_register(dev, &gate->hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register %s gate\n",
				     data->name);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					   &gate->hw);
}

static const struct sc9820e_peripheral_gate_data hwspinlock_gate = {
	.name = "sc9820e-hwspinlock",
	.syscon_property = "fplinux,aon-syscon",
	.offset = SC9820E_AON_APB_EB0,
	.mask = SC9820E_AON_APB_HWSPINLOCK,
};

static const struct sc9820e_peripheral_gate_data hs_spi_gate = {
	.name = "sc9820e-hs-spi-enable",
	.syscon_property = "fplinux,aon-syscon",
	.offset = SC9820E_AON_APB_HS_SPI_EB,
	.mask = SC9820E_AON_APB_HS_SPI,
};

static const struct sc9820e_peripheral_gate_data gpio_gate = {
	.name = "sc9820e-gpio",
	.syscon_property = "fplinux,aon-syscon",
	.offset = SC9820E_AON_APB_EB0,
	.mask = SC9820E_AON_APB_GPIO,
};

static const struct sc9820e_peripheral_gate_data efuse_gate = {
	.name = "sc9820e-efuse",
	.syscon_property = "fplinux,aon-syscon",
	.offset = SC9820E_AON_APB_EB0,
	.mask = SC9820E_AON_APB_EFUSE,
};

static const struct sc9820e_peripheral_gate_data mailbox_gate = {
	.name = "sc9820e-mailbox",
	.syscon_property = "fplinux,aon-syscon",
	.offset = SC9820E_AON_APB_EB1,
	.mask = SC9820E_AON_APB_MAILBOX,
};

static const struct sc9820e_peripheral_gate_data ap_dma_gate = {
	.name = "sc9820e-ap-dma-enable",
	.syscon_property = "fplinux,ap-ahb-syscon",
	.offset = SC9820E_AP_AHB_EB,
	.mask = SC9820E_AP_AHB_DMA,
};

static const struct of_device_id sc9820e_peripheral_gates_match[] = {
	{ .compatible = "fplinux,sc9820e-aon-gate", .data = &hwspinlock_gate },
	{ .compatible = "fplinux,sc9820e-hs-spi-gate", .data = &hs_spi_gate },
	{ .compatible = "fplinux,sc9820e-gpio-gate", .data = &gpio_gate },
	{ .compatible = "fplinux,sc9820e-efuse-gate", .data = &efuse_gate },
	{ .compatible = "fplinux,sc9820e-mailbox-gate", .data = &mailbox_gate },
	{ .compatible = "fplinux,sc9820e-ap-dma-gate", .data = &ap_dma_gate },
	{}
};
MODULE_DEVICE_TABLE(of, sc9820e_peripheral_gates_match);

static struct platform_driver sc9820e_peripheral_gates_driver = {
	.probe = sc9820e_peripheral_gate_probe,
	.driver = {
		.name = "sc9820e-peripheral-gates",
		.of_match_table = sc9820e_peripheral_gates_match,
	},
};
module_platform_driver(sc9820e_peripheral_gates_driver);

MODULE_DESCRIPTION("SC9820E peripheral clock gates");
MODULE_LICENSE("GPL");
