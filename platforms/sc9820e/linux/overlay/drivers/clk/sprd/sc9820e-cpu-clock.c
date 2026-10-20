// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/fplinux,sc9820e-cpu-clock.h>

#define SC9820E_AP_AHB_CA53_CKG_DIV_CFG 0x38
#define SC9820E_AP_AHB_CA53_CKG_SEL_CFG 0x54
#define SC9820E_CA53_MCU_MUX_MASK GENMASK(1, 0)
#define SC9820E_CA53_MCU_DIV_MASK GENMASK(6, 4)
#define SC9820E_PLL_CTRL0 0x0
#define SC9820E_PLL_CTRL1 0x4
#define SC9820E_PLL_CTRL0_DIV_S BIT(0)
#define SC9820E_PLL_CTRL0_SDM_EN BIT(2)
#define SC9820E_PLL_CTRL0_N_MASK GENMASK(18, 8)
#define SC9820E_PLL_CTRL1_NINT_MASK GENMASK(28, 23)
#define SC9820E_PLL_CTRL1_KINT_MASK GENMASK(22, 0)
#define SC9820E_CPU_REFERENCE_HZ 26000000

struct sc9820e_cpu_state {
	u32 selection;
	u32 divider;
	u32 mpll[2];
	u32 twpll[2];
};

struct sc9820e_cpu_clock {
	struct clk_hw hw;
	struct clk_hw twpll_hw;
	struct regmap *ap_ahb;
	void __iomem *mpll;
	void __iomem *twpll;
	struct mutex transition_lock;
	struct sc9820e_cpu_state inherited;
	struct cpumask cpus;
	struct cpufreq_frequency_table frequencies[3];
	struct cpufreq_driver cpufreq;
};

static u64 decode_pll_rate(u32 control0, u32 control1,
			   unsigned long reference_rate)
{
	u64 rate;
	u32 fraction;

	if (!(control0 & SC9820E_PLL_CTRL0_DIV_S))
		return (u64)reference_rate * 10 *
		       FIELD_GET(SC9820E_PLL_CTRL0_N_MASK, control0);

	rate = (u64)reference_rate *
	       FIELD_GET(SC9820E_PLL_CTRL1_NINT_MASK, control1);
	if (control0 & SC9820E_PLL_CTRL0_SDM_EN) {
		fraction = FIELD_GET(SC9820E_PLL_CTRL1_KINT_MASK, control1);
		rate += DIV_ROUND_CLOSEST_ULL((u64)reference_rate * fraction,
					      BIT(23));
	}

	return rate;
}

static u64 pll_rate(void __iomem *base, unsigned long reference_rate)
{
	u32 control0 = readl(base + SC9820E_PLL_CTRL0);
	u32 control1 = readl(base + SC9820E_PLL_CTRL1);

	return decode_pll_rate(control0, control1, reference_rate);
}

static unsigned long sc9820e_cpu_recalc_rate(struct clk_hw *hw,
					     unsigned long reference_rate)
{
	struct sc9820e_cpu_clock *clock =
		container_of(hw, struct sc9820e_cpu_clock, hw);
	u32 selection, divider;
	u64 rate;
	int ret;

	if (reference_rate != SC9820E_CPU_REFERENCE_HZ)
		return 0;

	ret = regmap_read(clock->ap_ahb, SC9820E_AP_AHB_CA53_CKG_SEL_CFG,
			  &selection);
	if (ret)
		return 0;
	ret = regmap_read(clock->ap_ahb, SC9820E_AP_AHB_CA53_CKG_DIV_CFG,
			  &divider);
	if (ret)
		return 0;

	switch (FIELD_GET(SC9820E_CA53_MCU_MUX_MASK, selection)) {
	case 0:
		rate = reference_rate;
		break;
	case 1:
		rate = div_u64(pll_rate(clock->twpll, reference_rate), 3);
		break;
	case 2:
		rate = div_u64(pll_rate(clock->twpll, reference_rate), 2);
		break;
	case 3:
		rate = pll_rate(clock->mpll, reference_rate);
		break;
	default:
		return 0;
	}

	divider = FIELD_GET(SC9820E_CA53_MCU_DIV_MASK, divider) + 1;
	rate = DIV_ROUND_UP_ULL(rate, divider);
	if (rate > ULONG_MAX)
		return 0;
	return rate;
}

static const struct clk_ops sc9820e_cpu_clock_ops = {
	.recalc_rate = sc9820e_cpu_recalc_rate,
};

static unsigned long sc9820e_twpll_recalc_rate(struct clk_hw *hw,
					       unsigned long reference_rate)
{
	struct sc9820e_cpu_clock *clock =
		container_of(hw, struct sc9820e_cpu_clock, twpll_hw);
	u64 rate;

	if (reference_rate != SC9820E_CPU_REFERENCE_HZ)
		return 0;
	rate = pll_rate(clock->twpll, reference_rate);
	return rate > ULONG_MAX ? 0 : rate;
}

static const struct clk_ops sc9820e_twpll_clock_ops = {
	.recalc_rate = sc9820e_twpll_recalc_rate,
};

static int read_cpu_state(struct sc9820e_cpu_clock *clock,
			  struct sc9820e_cpu_state *state)
{
	int ret;

	ret = regmap_read(clock->ap_ahb, SC9820E_AP_AHB_CA53_CKG_SEL_CFG,
			  &state->selection);
	if (ret)
		return ret;
	ret = regmap_read(clock->ap_ahb, SC9820E_AP_AHB_CA53_CKG_DIV_CFG,
			  &state->divider);
	if (ret)
		return ret;
	state->mpll[0] = readl(clock->mpll + SC9820E_PLL_CTRL0);
	state->mpll[1] = readl(clock->mpll + SC9820E_PLL_CTRL1);
	state->twpll[0] = readl(clock->twpll + SC9820E_PLL_CTRL0);
	state->twpll[1] = readl(clock->twpll + SC9820E_PLL_CTRL1);
	return 0;
}

static bool preserved_cpu_state(struct sc9820e_cpu_clock *clock,
				const struct sc9820e_cpu_state *state)
{
	const struct sc9820e_cpu_state *inherited = &clock->inherited;
	u32 preserved_mask = ~SC9820E_CA53_MCU_MUX_MASK;
	u32 parent = FIELD_GET(SC9820E_CA53_MCU_MUX_MASK, state->selection);

	return (parent == 2 || parent == 3) &&
	       (state->selection & preserved_mask) ==
		       (inherited->selection & preserved_mask) &&
	       state->divider == inherited->divider &&
	       state->mpll[0] == inherited->mpll[0] &&
	       state->mpll[1] == inherited->mpll[1] &&
	       state->twpll[0] == inherited->twpll[0] &&
	       state->twpll[1] == inherited->twpll[1];
}

static unsigned int state_frequency(struct sc9820e_cpu_clock *clock,
				    const struct sc9820e_cpu_state *state)
{
	u32 parent = FIELD_GET(SC9820E_CA53_MCU_MUX_MASK, state->selection);

	return clock->frequencies[parent == 3].frequency;
}

static unsigned int sc9820e_cpufreq_get(unsigned int cpu)
{
	struct sc9820e_cpu_clock *clock = cpufreq_get_driver_data();
	struct sc9820e_cpu_state state;
	unsigned int frequency = 0;

	if (!clock || !cpumask_test_cpu(cpu, &clock->cpus))
		return 0;
	mutex_lock(&clock->transition_lock);
	if (!read_cpu_state(clock, &state) &&
	    preserved_cpu_state(clock, &state))
		frequency = state_frequency(clock, &state);
	mutex_unlock(&clock->transition_lock);
	return frequency;
}

static int sc9820e_cpufreq_target(struct cpufreq_policy *policy,
				  unsigned int index)
{
	struct sc9820e_cpu_clock *clock = policy->driver_data;
	struct sc9820e_cpu_state state;
	u32 parent;
	int ret, read_ret;

	if (index >= 2)
		return -EINVAL;
	parent = clock->frequencies[index].driver_data;
	mutex_lock(&clock->transition_lock);
	ret = read_cpu_state(clock, &state);
	if (ret)
		goto out;
	if (!preserved_cpu_state(clock, &state)) {
		ret = -EIO;
		goto out;
	}
	if (FIELD_GET(SC9820E_CA53_MCU_MUX_MASK, state.selection) == parent)
		goto out;

	ret = regmap_update_bits(clock->ap_ahb, SC9820E_AP_AHB_CA53_CKG_SEL_CFG,
				 SC9820E_CA53_MCU_MUX_MASK, parent);
	read_ret = read_cpu_state(clock, &state);
	if (!ret) {
		if (read_ret)
			ret = read_ret;
		else if (!preserved_cpu_state(clock, &state) ||
			 FIELD_GET(SC9820E_CA53_MCU_MUX_MASK,
				   state.selection) != parent)
			ret = -EIO;
	}
out:
	mutex_unlock(&clock->transition_lock);
	return ret;
}

static int sc9820e_cpufreq_init(struct cpufreq_policy *policy)
{
	struct sc9820e_cpu_clock *clock = cpufreq_get_driver_data();
	unsigned int frequency = sc9820e_cpufreq_get(policy->cpu);

	if (!frequency)
		return -EIO;
	cpumask_copy(policy->cpus, &clock->cpus);
	policy->driver_data = clock;
	policy->freq_table = clock->frequencies;
	policy->cur = frequency;
	policy->cpuinfo.transition_latency =
		CPUFREQ_DEFAULT_TRANSITION_LATENCY_NS;
	policy->dvfs_possible_from_any_cpu = true;
	return 0;
}

static const struct cpufreq_driver sc9820e_cpufreq_driver = {
	.name = "sc9820e-parent",
	.flags = CPUFREQ_CONST_LOOPS,
	.init = sc9820e_cpufreq_init,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = sc9820e_cpufreq_target,
	.get = sc9820e_cpufreq_get,
};

static void unregister_cpufreq(void *data)
{
	struct sc9820e_cpu_clock *clock = data;

	cpufreq_unregister_driver(&clock->cpufreq);
}

static bool cpu_uses_clock(unsigned int cpu, struct device_node *clock_node)
{
	struct device *cpu_dev = get_cpu_device(cpu);
	struct of_phandle_args args;
	bool matches;

	if (!cpu_dev)
		return false;
	if (of_parse_phandle_with_args(cpu_dev->of_node, "clocks",
				       "#clock-cells", 0, &args))
		return false;
	matches = args.np == clock_node && args.args_count == 1 &&
		  args.args[0] == SC9820E_CPU_CLK_CPU;
	of_node_put(args.np);
	return matches;
}

static int register_cpufreq(struct sc9820e_cpu_clock *clock, struct device *dev)
{
	struct sc9820e_cpu_state state;
	struct clk_hw *reference;
	u64 high_rate, low_rate;
	unsigned int cpu;
	int ret;

	if (!cpu_uses_clock(0, dev->of_node))
		return 0;
	for_each_possible_cpu(cpu)
		if (cpu_uses_clock(cpu, dev->of_node))
			cpumask_set_cpu(cpu, &clock->cpus);
	if (cpumask_weight(&clock->cpus) != 2)
		return -EINVAL;
	reference = clk_hw_get_parent(&clock->hw);
	if (!reference ||
	    clk_hw_get_rate(reference) != SC9820E_CPU_REFERENCE_HZ)
		return -EINVAL;

	ret = read_cpu_state(clock, &clock->inherited);
	if (ret)
		return ret;
	if (FIELD_GET(SC9820E_CA53_MCU_MUX_MASK, clock->inherited.selection) !=
		    3 ||
	    FIELD_GET(SC9820E_CA53_MCU_DIV_MASK, clock->inherited.divider))
		return -EINVAL;
	high_rate = decode_pll_rate(clock->inherited.mpll[0],
				    clock->inherited.mpll[1],
				    SC9820E_CPU_REFERENCE_HZ);
	low_rate = div_u64(decode_pll_rate(clock->inherited.twpll[0],
					   clock->inherited.twpll[1],
					   SC9820E_CPU_REFERENCE_HZ),
			   2);
	if (!low_rate || high_rate > ULONG_MAX || low_rate >= high_rate)
		return -EINVAL;
	clock->frequencies[0].frequency = DIV_ROUND_CLOSEST_ULL(low_rate, 1000);
	clock->frequencies[0].driver_data = 2;
	clock->frequencies[1].frequency =
		DIV_ROUND_CLOSEST_ULL(high_rate, 1000);
	clock->frequencies[1].driver_data = 3;
	clock->frequencies[2].frequency = CPUFREQ_TABLE_END;
	if (!clock->frequencies[0].frequency ||
	    clock->frequencies[0].frequency >= clock->frequencies[1].frequency)
		return -EINVAL;
	ret = read_cpu_state(clock, &state);
	if (ret)
		return ret;
	if (!preserved_cpu_state(clock, &state) ||
	    state.selection != clock->inherited.selection)
		return -EIO;

	mutex_init(&clock->transition_lock);
	clock->cpufreq = sc9820e_cpufreq_driver;
	clock->cpufreq.driver_data = clock;
	ret = cpufreq_register_driver(&clock->cpufreq);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, unregister_cpufreq, clock);
}

static void __iomem *map_pll(struct platform_device *pdev, const char *name)
{
	struct resource *resource;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource_size(resource) != 2 * sizeof(u32))
		return IOMEM_ERR_PTR(
			dev_err_probe(&pdev->dev, -EINVAL,
				      "expected two %s registers\n", name));

	return devm_platform_ioremap_resource_byname(pdev, name);
}

static int sc9820e_cpu_clock_probe(struct platform_device *pdev)
{
	const struct clk_parent_data parent = { .fw_name = "source" };
	struct clk_init_data init = {
		.name = "sc9820e-cpu",
		.ops = &sc9820e_cpu_clock_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	struct clk_init_data twpll_init = {
		.name = "sc9820e-twpll",
		.ops = &sc9820e_twpll_clock_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	struct device *dev = &pdev->dev;
	struct sc9820e_cpu_clock *clock;
	struct clk_hw_onecell_data *clocks;
	int ret;

	clock = devm_kzalloc(dev, sizeof(*clock), GFP_KERNEL);
	if (!clock)
		return -ENOMEM;
	clocks = devm_kzalloc(dev,
			      struct_size(clocks, hws, SC9820E_CPU_CLK_COUNT),
			      GFP_KERNEL);
	if (!clocks)
		return -ENOMEM;
	clocks->num = SC9820E_CPU_CLK_COUNT;

	clock->ap_ahb = syscon_regmap_lookup_by_phandle(
		dev->of_node, "fplinux,ap-ahb-syscon");
	if (IS_ERR(clock->ap_ahb))
		return dev_err_probe(dev, PTR_ERR(clock->ap_ahb),
				     "failed to get AP-AHB register map\n");

	clock->mpll = map_pll(pdev, "mpll");
	if (IS_ERR(clock->mpll))
		return PTR_ERR(clock->mpll);
	clock->twpll = map_pll(pdev, "twpll");
	if (IS_ERR(clock->twpll))
		return PTR_ERR(clock->twpll);

	clock->hw.init = &init;
	ret = devm_clk_hw_register(dev, &clock->hw);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register CPU clock\n");
	clocks->hws[SC9820E_CPU_CLK_CPU] = &clock->hw;

	clock->twpll_hw.init = &twpll_init;
	ret = devm_clk_hw_register(dev, &clock->twpll_hw);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register TWPLL clock\n");
	clocks->hws[SC9820E_CPU_CLK_TWPLL] = &clock->twpll_hw;

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clocks);
	if (ret)
		return ret;
	ret = register_cpufreq(clock, dev);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"failed to register CPU parent frequency control\n");
	return 0;
}

static const struct of_device_id sc9820e_cpu_clock_match[] = {
	{ .compatible = "fplinux,sc9820e-cpu-clock" },
	{}
};
MODULE_DEVICE_TABLE(of, sc9820e_cpu_clock_match);

static struct platform_driver sc9820e_cpu_clock_driver = {
	.probe = sc9820e_cpu_clock_probe,
	.driver = {
		.name = "sc9820e-cpu-clock",
		.of_match_table = sc9820e_cpu_clock_match,
	},
};
module_platform_driver(sc9820e_cpu_clock_driver);

MODULE_DESCRIPTION("SC9820E inherited CPU clock and parent frequency control");
MODULE_LICENSE("GPL");
