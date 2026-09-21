// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define SC2720_XTL_WAIT_CTRL0 0xde8
#define SC2720_XTL_WAIT_CTRL0_EN BIT(8)
#define SC2720_XTL_RATE_HZ 26000000

struct sc2720_clk {
	struct clk_hw hw;
	struct device *dev;
	struct regmap *regmap;
	bool owned;
};

static int sc2720_clk_prepare(struct clk_hw *hw)
{
	struct sc2720_clk *clock = container_of(hw, struct sc2720_clk, hw);
	unsigned int value;
	int ret;

	ret = regmap_read(clock->regmap, SC2720_XTL_WAIT_CTRL0, &value);
	if (ret)
		return ret;
	/* A request already held by firmware must survive the last Linux user. */
	if (!clock->owned)
		clock->owned = !(value & SC2720_XTL_WAIT_CTRL0_EN);
	if (!clock->owned)
		return 0;
	return regmap_update_bits(clock->regmap, SC2720_XTL_WAIT_CTRL0,
				  SC2720_XTL_WAIT_CTRL0_EN,
				  SC2720_XTL_WAIT_CTRL0_EN);
}

static void sc2720_clk_unprepare(struct clk_hw *hw)
{
	struct sc2720_clk *clock = container_of(hw, struct sc2720_clk, hw);
	int ret;

	if (!clock->owned)
		return;
	ret = regmap_update_bits(clock->regmap, SC2720_XTL_WAIT_CTRL0,
				 SC2720_XTL_WAIT_CTRL0_EN, 0);
	if (ret)
		dev_err(clock->dev, "cannot release crystal request: %pe\n",
			ERR_PTR(ret));
	else
		clock->owned = false;
}

static int sc2720_clk_is_prepared(struct clk_hw *hw)
{
	struct sc2720_clk *clock = container_of(hw, struct sc2720_clk, hw);
	unsigned int value;
	int ret;

	ret = regmap_read(clock->regmap, SC2720_XTL_WAIT_CTRL0, &value);
	return ret ? ret : !!(value & SC2720_XTL_WAIT_CTRL0_EN);
}

static unsigned long sc2720_clk_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	return SC2720_XTL_RATE_HZ;
}

static const struct clk_ops sc2720_clk_ops = {
	.prepare = sc2720_clk_prepare,
	.unprepare = sc2720_clk_unprepare,
	.is_prepared = sc2720_clk_is_prepared,
	.recalc_rate = sc2720_clk_recalc_rate,
};

static int sc2720_clk_probe(struct platform_device *pdev)
{
	struct clk_init_data init = {
		.name = "sc2720-26m",
		.ops = &sc2720_clk_ops,
		.flags = CLK_IGNORE_UNUSED,
	};
	struct sc2720_clk *clock;
	int ret;

	clock = devm_kzalloc(&pdev->dev, sizeof(*clock), GFP_KERNEL);
	if (!clock)
		return -ENOMEM;
	clock->dev = &pdev->dev;
	clock->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!clock->regmap)
		return -EPROBE_DEFER;
	clock->hw.init = &init;
	ret = devm_clk_hw_register(&pdev->dev, &clock->hw);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_simple_get,
					   &clock->hw);
}

static const struct of_device_id sc2720_clk_match[] = {
	{ .compatible = "sprd,sc2720-clk" },
	{}
};
MODULE_DEVICE_TABLE(of, sc2720_clk_match);

static struct platform_driver sc2720_clk_driver = {
	.probe = sc2720_clk_probe,
	.driver = {
		.name = "sc2720-clk",
		.of_match_table = sc2720_clk_match,
	},
};
module_platform_driver(sc2720_clk_driver);

MODULE_DESCRIPTION("SC2720 shared 26 MHz crystal request");
MODULE_LICENSE("GPL");
