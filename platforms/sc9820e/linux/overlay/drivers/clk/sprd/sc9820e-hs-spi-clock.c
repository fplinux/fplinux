// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define SC9820E_HS_SPI_MUX_MASK GENMASK(1, 0)
#define SC9820E_HS_SPI_DIV_MASK GENMASK(10, 8)
#define SC9820E_HS_SPI_DIV_SHIFT 8

struct sc9820e_hs_spi_clock {
	struct clk_hw hw;
	void __iomem *base;
};

static struct sc9820e_hs_spi_clock *to_spi_clock(struct clk_hw *hw)
{
	return container_of(hw, struct sc9820e_hs_spi_clock, hw);
}

static unsigned long sc9820e_hs_spi_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct sc9820e_hs_spi_clock *clock = to_spi_clock(hw);
	u32 value = readl(clock->base);
	u32 divisor;

	divisor = ((value & SC9820E_HS_SPI_DIV_MASK) >>
		   SC9820E_HS_SPI_DIV_SHIFT) +
		  1;
	return parent_rate / divisor;
}

static u8 sc9820e_hs_spi_get_parent(struct clk_hw *hw)
{
	struct sc9820e_hs_spi_clock *clock = to_spi_clock(hw);

	return readl(clock->base) & SC9820E_HS_SPI_MUX_MASK;
}

static int sc9820e_hs_spi_set_parent(struct clk_hw *hw, u8 index)
{
	struct sc9820e_hs_spi_clock *clock = to_spi_clock(hw);
	u32 value;

	if (index > SC9820E_HS_SPI_MUX_MASK)
		return -EINVAL;
	value = readl(clock->base);
	value &= ~SC9820E_HS_SPI_MUX_MASK;
	writel(value | index, clock->base);
	return (readl(clock->base) & SC9820E_HS_SPI_MUX_MASK) == index ? 0 :
									 -EIO;
}

static int sc9820e_hs_spi_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	struct clk_hw *parent = clk_hw_get_parent(hw);
	unsigned long parent_rate;

	if (!parent)
		return -EINVAL;

	parent_rate = clk_hw_get_rate(parent);
	if (!parent_rate || parent_rate < req->min_rate ||
	    parent_rate > req->max_rate)
		return -EINVAL;

	req->best_parent_hw = parent;
	req->best_parent_rate = parent_rate;
	req->rate = parent_rate;
	return 0;
}

static int sc9820e_hs_spi_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct sc9820e_hs_spi_clock *clock = to_spi_clock(hw);
	u32 value;

	if (!parent_rate || rate != parent_rate)
		return -EINVAL;

	value = readl(clock->base);
	writel(value & ~SC9820E_HS_SPI_DIV_MASK, clock->base);
	return (readl(clock->base) & SC9820E_HS_SPI_DIV_MASK) ? -EIO : 0;
}

static const struct clk_ops sc9820e_hs_spi_clock_ops = {
	.recalc_rate = sc9820e_hs_spi_recalc_rate,
	.get_parent = sc9820e_hs_spi_get_parent,
	.set_parent = sc9820e_hs_spi_set_parent,
	.determine_rate = sc9820e_hs_spi_determine_rate,
	.set_rate = sc9820e_hs_spi_set_rate,
};

static int sc9820e_hs_spi_clock_probe(struct platform_device *pdev)
{
	static const struct clk_parent_data parents[] = {
		{ .fw_name = "ext26m" },
		{ .fw_name = "twpll128m" },
		{ .fw_name = "twpll153m6" },
		{ .fw_name = "twpll192m" },
	};
	struct clk_init_data init = {
		.name = "sc9820e-hs-spi",
		.ops = &sc9820e_hs_spi_clock_ops,
		.parent_data = parents,
		.num_parents = ARRAY_SIZE(parents),
		.flags = CLK_GET_RATE_NOCACHE,
	};
	struct device *dev = &pdev->dev;
	struct sc9820e_hs_spi_clock *clock;
	struct resource *resource;
	int ret;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource_size(resource) != sizeof(u32))
		return dev_err_probe(dev, -EINVAL,
				     "expected one clock register\n");

	clock = devm_kzalloc(dev, sizeof(*clock), GFP_KERNEL);
	if (!clock)
		return -ENOMEM;

	clock->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(clock->base))
		return PTR_ERR(clock->base);

	clock->hw.init = &init;
	ret = devm_clk_hw_register(dev, &clock->hw);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register SPI clock\n");

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					   &clock->hw);
}

static const struct of_device_id sc9820e_hs_spi_clock_match[] = {
	{ .compatible = "fplinux,sc9820e-hs-spi-clock" },
	{}
};
MODULE_DEVICE_TABLE(of, sc9820e_hs_spi_clock_match);

static struct platform_driver sc9820e_hs_spi_clock_driver = {
	.probe = sc9820e_hs_spi_clock_probe,
	.driver = {
		.name = "sc9820e-hs-spi-clock",
		.of_match_table = sc9820e_hs_spi_clock_match,
	},
};
module_platform_driver(sc9820e_hs_spi_clock_driver);

MODULE_DESCRIPTION("SC9820E high-speed SPI source mux and divider");
MODULE_LICENSE("GPL");
