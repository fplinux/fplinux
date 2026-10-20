// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define SC9820E_GPU_TOP_CFG 0x20
#define SC9820E_GPU_TOP_MASK GENMASK(25, 0)
#define SC9820E_GPU_TOP_DELAYS 0x00010101
#define SC9820E_GPU_AON_EB0 0x0
#define SC9820E_GPU_AON_GATE BIT(27)
#define SC9820E_GPU_AON_SET 0x1000
#define SC9820E_GPU_AON_CLEAR 0x2000
#define SC9820E_GPU_MUX_MASK GENMASK(2, 0)
#define SC9820E_GPU_DIV_MASK GENMASK(6, 4)
#define SC9820E_GPU_DIV_SHIFT 4
#define SC9820E_GPU_QOS_MASK GENMASK(15, 8)
#define SC9820E_GPU_QOS_VALUE 0x6600
#define SC9820E_GPU_SETTLE_US 300

struct sc9820e_gpu_rate {
	unsigned long nominal_rate_hz;
	u32 source_divisor;
	u32 mux;
};

static const struct sc9820e_gpu_rate gpu_rates[] = {
	{ 256000000, 6, 0 },
	{ 384000000, 4, 2 },
	{ 512000000, 3, 3 },
};

struct sc9820e_gpu_clock {
	struct clk_hw hw;
	struct device *dev;
	struct regmap *pmu;
	struct regmap *aon;
	struct regulator *supply;
	struct clk *source;
	void __iomem *clock;
	void __iomem *qos;
	void __iomem *reset;
	u32 saved_pmu;
	u32 saved_gate;
	u32 saved_clock;
	u32 saved_qos;
	u32 expected_core_uv;
	unsigned int rate_index;
	bool pmu_claimed;
	bool source_enabled;
	bool gate_claimed;
	bool registers_saved;
	bool active;
};

static int restore_gpu_clock(struct sc9820e_gpu_clock *gpu)
{
	u32 value;
	int ret = 0, error;

	gpu->active = false;
	if (gpu->registers_saved) {
		value = readl(gpu->qos);
		writel((value & ~SC9820E_GPU_QOS_MASK) |
			       (gpu->saved_qos & SC9820E_GPU_QOS_MASK),
		       gpu->qos);
		if ((readl(gpu->qos) ^ gpu->saved_qos) & SC9820E_GPU_QOS_MASK)
			ret = -EIO;
		value = readl(gpu->clock);
		writel((value & ~SC9820E_GPU_MUX_MASK) |
			       (gpu->saved_clock & SC9820E_GPU_MUX_MASK),
		       gpu->clock);
		if ((readl(gpu->clock) ^ gpu->saved_clock) &
		    SC9820E_GPU_MUX_MASK)
			ret = -EIO;
		gpu->registers_saved = false;
	}
	if (gpu->gate_claimed) {
		error = regmap_write(gpu->aon, SC9820E_GPU_AON_CLEAR,
				     SC9820E_GPU_AON_GATE);
		if (!error)
			error = regmap_read(gpu->aon, SC9820E_GPU_AON_EB0,
					    &value);
		if (!error && (value & SC9820E_GPU_AON_GATE))
			error = -EIO;
		if (error && !ret)
			ret = error;
		gpu->gate_claimed = false;
	}
	if (gpu->source_enabled) {
		clk_disable(gpu->source);
		gpu->source_enabled = false;
	}
	if (gpu->pmu_claimed) {
		error = regmap_update_bits(gpu->pmu, SC9820E_GPU_TOP_CFG,
					   SC9820E_GPU_TOP_MASK,
					   gpu->saved_pmu);
		if (!error)
			error = regmap_read(gpu->pmu, SC9820E_GPU_TOP_CFG,
					    &value);
		if (!error && ((value ^ gpu->saved_pmu) & SC9820E_GPU_TOP_MASK))
			error = -EIO;
		if (error && !ret)
			ret = error;
		gpu->pmu_claimed = false;
	}
	return ret;
}

static int sc9820e_gpu_clock_prepare(struct clk_hw *hw)
{
	struct sc9820e_gpu_clock *gpu =
		container_of(hw, struct sc9820e_gpu_clock, hw);
	u32 value;
	int ret, restore_ret;

	/* The shared rail is observed, never reprogrammed by this provider. */
	ret = regulator_get_voltage(gpu->supply);
	if (ret < 0)
		return ret;
	if ((u32)ret != gpu->expected_core_uv || !clk_get_rate(gpu->source))
		return -EINVAL;
	ret = regmap_read(gpu->pmu, SC9820E_GPU_TOP_CFG, &gpu->saved_pmu);
	if (ret)
		return ret;
	ret = regmap_read(gpu->aon, SC9820E_GPU_AON_EB0, &gpu->saved_gate);
	if (ret)
		return ret;

	/* A write reporting failure may still have changed the hardware. */
	gpu->pmu_claimed = true;
	ret = regmap_update_bits(gpu->pmu, SC9820E_GPU_TOP_CFG,
				 SC9820E_GPU_TOP_MASK, SC9820E_GPU_TOP_DELAYS);
	if (ret)
		goto restore;
	ret = regmap_read(gpu->pmu, SC9820E_GPU_TOP_CFG, &value);
	if (ret)
		goto restore;
	if ((value & SC9820E_GPU_TOP_MASK) != SC9820E_GPU_TOP_DELAYS) {
		ret = -EIO;
		goto restore;
	}
	udelay(SC9820E_GPU_SETTLE_US);

	/* CCF has prepared the parent before entering this callback. */
	ret = clk_enable(gpu->source);
	if (ret)
		goto restore;
	gpu->source_enabled = true;
	if (!(gpu->saved_gate & SC9820E_GPU_AON_GATE)) {
		gpu->gate_claimed = true;
		ret = regmap_write(gpu->aon, SC9820E_GPU_AON_SET,
				   SC9820E_GPU_AON_GATE);
		if (ret)
			goto restore;
	}
	ret = regmap_read(gpu->aon, SC9820E_GPU_AON_EB0, &value);
	if (ret)
		goto restore;
	if (!(value & SC9820E_GPU_AON_GATE)) {
		ret = -EIO;
		goto restore;
	}
	udelay(SC9820E_GPU_SETTLE_US);

	/* No control-bank access is allowed before domain, source and gate. */
	gpu->saved_clock = readl(gpu->clock);
	gpu->saved_qos = readl(gpu->qos);
	if (readl(gpu->reset) & BIT(0)) {
		ret = -EBUSY;
		goto restore;
	}
	if (gpu->saved_clock & SC9820E_GPU_DIV_MASK) {
		ret = -EINVAL;
		goto restore;
	}
	gpu->registers_saved = true;
	value = (gpu->saved_clock & ~SC9820E_GPU_MUX_MASK) |
		gpu_rates[gpu->rate_index].mux;
	writel(value, gpu->clock);
	if (readl(gpu->clock) != value) {
		ret = -EIO;
		goto restore;
	}
	writel((gpu->saved_qos & ~SC9820E_GPU_QOS_MASK) | SC9820E_GPU_QOS_VALUE,
	       gpu->qos);
	value = readl(gpu->qos);
	if (value != ((gpu->saved_qos & ~SC9820E_GPU_QOS_MASK) |
		      SC9820E_GPU_QOS_VALUE)) {
		ret = -EIO;
		goto restore;
	}
	gpu->active = true;
	return 0;

restore:
	restore_ret = restore_gpu_clock(gpu);
	return restore_ret ? restore_ret : ret;
}

static void sc9820e_gpu_clock_unprepare(struct clk_hw *hw)
{
	struct sc9820e_gpu_clock *gpu =
		container_of(hw, struct sc9820e_gpu_clock, hw);
	int ret = restore_gpu_clock(gpu);

	if (ret)
		dev_err(gpu->dev, "failed to restore GPU clock resources: %d\n",
			ret);
}

static unsigned long sc9820e_gpu_clock_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct sc9820e_gpu_clock *gpu =
		container_of(hw, struct sc9820e_gpu_clock, hw);
	unsigned int index;
	u32 value, divider;

	/* The inherited mux is unknown until the gated bank is prepared. */
	if (!gpu->active)
		return 0;
	value = readl(gpu->clock);
	divider = ((value & SC9820E_GPU_DIV_MASK) >> SC9820E_GPU_DIV_SHIFT) + 1;
	for (index = 0; index < ARRAY_SIZE(gpu_rates); index++)
		if ((value & SC9820E_GPU_MUX_MASK) == gpu_rates[index].mux)
			return parent_rate / gpu_rates[index].source_divisor /
			       divider;
	return 0;
}

static int sc9820e_gpu_clock_determine_rate(struct clk_hw *hw,
					    struct clk_rate_request *request)
{
	unsigned int index;
	unsigned long actual_rate;

	(void)hw;
	if (!request->best_parent_rate)
		return -EINVAL;
	for (index = 0; index < ARRAY_SIZE(gpu_rates); index++) {
		actual_rate = request->best_parent_rate /
			      gpu_rates[index].source_divisor;
		if (!actual_rate || actual_rate < request->min_rate ||
		    actual_rate > request->max_rate)
			continue;
		if (request->rate == gpu_rates[index].nominal_rate_hz ||
		    request->rate == actual_rate) {
			request->rate = actual_rate;
			return 0;
		}
	}
	return -EINVAL;
}

static int sc9820e_gpu_clock_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct sc9820e_gpu_clock *gpu =
		container_of(hw, struct sc9820e_gpu_clock, hw);
	unsigned int index;

	if (gpu->active)
		return -EBUSY;
	if (!rate || !parent_rate)
		return -EINVAL;
	for (index = 0; index < ARRAY_SIZE(gpu_rates); index++) {
		if (rate != parent_rate / gpu_rates[index].source_divisor)
			continue;
		/* The gated mux is programmed only by prepare, before GPU jobs. */
		gpu->rate_index = index;
		return 0;
	}
	return -EINVAL;
}

static const struct clk_ops sc9820e_gpu_clock_ops = {
	.prepare = sc9820e_gpu_clock_prepare,
	.unprepare = sc9820e_gpu_clock_unprepare,
	.recalc_rate = sc9820e_gpu_clock_recalc_rate,
	.determine_rate = sc9820e_gpu_clock_determine_rate,
	.set_rate = sc9820e_gpu_clock_set_rate,
};

static void __iomem *map_gpu_word(struct platform_device *pdev,
				  const char *name)
{
	struct resource *resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!resource || resource_size(resource) != sizeof(u32))
		return IOMEM_ERR_PTR(-EINVAL);
	return devm_ioremap_resource(&pdev->dev, resource);
}

static int sc9820e_gpu_clock_probe(struct platform_device *pdev)
{
	const struct clk_parent_data parent = { .fw_name = "source" };
	struct clk_init_data init = {
		.name = "sc9820e-gpu",
		.ops = &sc9820e_gpu_clock_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_GATE,
	};
	struct device *dev = &pdev->dev;
	struct sc9820e_gpu_clock *gpu;
	int ret;

	gpu = devm_kzalloc(dev, sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;
	gpu->dev = dev;
	ret = device_property_read_u32(dev, "fplinux,expected-core-microvolt",
				       &gpu->expected_core_uv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "cannot get expected core voltage\n");
	if (!gpu->expected_core_uv)
		return -EINVAL;
	gpu->pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "fplinux,pmu-syscon");
	if (IS_ERR(gpu->pmu))
		return dev_err_probe(dev, PTR_ERR(gpu->pmu),
				     "cannot get shared PMU registers\n");
	gpu->aon = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "fplinux,aon-syscon");
	if (IS_ERR(gpu->aon))
		return dev_err_probe(dev, PTR_ERR(gpu->aon),
				     "cannot get shared AON registers\n");
	gpu->source = devm_clk_get(dev, "source");
	if (IS_ERR(gpu->source))
		return dev_err_probe(dev, PTR_ERR(gpu->source),
				     "cannot get TWPLL source\n");
	gpu->supply = devm_regulator_get(dev, "core");
	if (IS_ERR(gpu->supply))
		return dev_err_probe(dev, PTR_ERR(gpu->supply),
				     "cannot get shared core supply\n");
	gpu->clock = map_gpu_word(pdev, "clock");
	if (IS_ERR(gpu->clock))
		return PTR_ERR(gpu->clock);
	gpu->qos = map_gpu_word(pdev, "qos");
	if (IS_ERR(gpu->qos))
		return PTR_ERR(gpu->qos);
	gpu->reset = map_gpu_word(pdev, "reset");
	if (IS_ERR(gpu->reset))
		return PTR_ERR(gpu->reset);
	gpu->hw.init = &init;
	ret = devm_clk_hw_register(dev, &gpu->hw);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &gpu->hw);
}

static const struct of_device_id sc9820e_gpu_clock_match[] = {
	{ .compatible = "fplinux,sc9820e-gpu-clock" },
	{}
};
MODULE_DEVICE_TABLE(of, sc9820e_gpu_clock_match);

static struct platform_driver sc9820e_gpu_clock_driver = {
	.probe = sc9820e_gpu_clock_probe,
	.driver = {
		.name = "sc9820e-gpu-clock",
		.of_match_table = sc9820e_gpu_clock_match,
	},
};
module_platform_driver(sc9820e_gpu_clock_driver);

MODULE_DESCRIPTION("SC9820E GPU clock and access preparation");
MODULE_LICENSE("GPL");
