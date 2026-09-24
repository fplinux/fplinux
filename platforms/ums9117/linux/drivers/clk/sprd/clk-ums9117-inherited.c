// SPDX-License-Identifier: GPL-2.0-only
/* Inherited UMS9117 clocks and fixed-voltage Cortex-A7 source selection. */
#include <dt-bindings/clock/fplinux,ums9117-inherited-clk.h>

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>

#define UMS9117_CA7_DIVIDER_PHYS 0x20e00038ULL
#define UMS9117_CA7_DIVIDER_BYTES 0x4ULL
#define UMS9117_CA7_SOURCE_PHYS 0x20e00054ULL
#define UMS9117_CA7_SOURCE_BYTES 0x4ULL
#define UMS9117_MPLL_PHYS 0x403f0000ULL
#define UMS9117_MPLL_BYTES 0xcULL
#define UMS9117_ANALOG_TOP_PHYS 0x403e0000ULL
#define UMS9117_ANALOG_TOP_BYTES 0x10ULL

#define UMS9117_CA7_DIVIDER_MASK GENMASK(6, 4)
#define UMS9117_CA7_SOURCE_MASK GENMASK(1, 0)

#define UMS9117_MPLL_CTRL0 0x0
#define UMS9117_MPLL_CTRL1 0x4
#define UMS9117_MPLL_CTRL2 0x8
#define UMS9117_MPLL_DIV_S BIT(0)
#define UMS9117_MPLL_SDM_EN BIT(2)
#define UMS9117_MPLL_NINT_MASK GENMASK(29, 23)
#define UMS9117_MPLL_KINT_MASK GENMASK(22, 0)
#define UMS9117_MPLL_POSTDIV BIT(0)

#define UMS9117_ANALOG_TOP_SEL0 0x0
#define UMS9117_ANALOG_TOP_SEL1 0x4
#define UMS9117_ANALOG_TOP_CTRL0 0x8
#define UMS9117_ANALOG_TOP_CTRL1 0xc
#define UMS9117_ANALOG_TOP_SEL0_TWPLL_REF BIT(18)
#define UMS9117_ANALOG_TOP_SEL0_TWPLL_PD BIT(17)
#define UMS9117_ANALOG_TOP_SEL0_TWPLL_RST BIT(13)
#define UMS9117_ANALOG_TOP_SEL0_TWPLL_DIV2 BIT(9)
#define UMS9117_ANALOG_TOP_CTRL0_TWPLL_LOCK_DONE BIT(6)

#define UMS9117_PMU_TWPLL_REL 0xa0
#define UMS9117_PMU_TWPLL_REL_REF_MASK GENMASK(9, 8)
#define UMS9117_PMU_TWPLL_REL_REF_RPLL 1
#define UMS9117_PMU_TWPLL_REL_FORCE_OFF BIT(7)
#define UMS9117_PMU_TWPLL_REL_AP_SEL BIT(0)

#define UMS9117_XTL_HZ 26000000ULL
#define UMS9117_TWPLL_512M_HZ 512000000UL
#define UMS9117_TWPLL_768M_HZ 768000000UL
#define UMS9117_MPLL_1G_HZ 1000000000UL
#define UMS9117_MPLL_KINT_SCALE BIT_ULL(23)

enum ums9117_ca7_source {
	UMS9117_CA7_SOURCE_XTL,
	UMS9117_CA7_SOURCE_TWPLL_512M,
	UMS9117_CA7_SOURCE_TWPLL_768M,
	UMS9117_CA7_SOURCE_MPLL,
};

struct ums9117_inherited_clks;

struct ums9117_observed_clk {
	struct clk_hw hw;
	struct ums9117_inherited_clks *provider;
};

struct ums9117_clock_snapshot {
	u32 ca7_source;
	u32 ca7_divider;
	u32 mpll_ctrl0;
	u32 mpll_ctrl1;
	u32 mpll_ctrl2;
};

struct ums9117_inherited_clks {
	struct device *dev;
	void __iomem *ca7_divider;
	void __iomem *ca7_source;
	void __iomem *mpll;
	void __iomem *analog_top;
	struct regmap *pmu;
	struct ums9117_observed_clk mpll_clk;
	struct ums9117_observed_clk ca7_clk;
	struct clk_hw_onecell_data *hw_data;
};

#define to_ums9117_observed_clk(_hw) \
	container_of(_hw, struct ums9117_observed_clk, hw)

static void
ums9117_read_clock_snapshot(const struct ums9117_inherited_clks *provider,
			    struct ums9117_clock_snapshot *snapshot)
{
	snapshot->ca7_source = readl(provider->ca7_source);
	snapshot->ca7_divider = readl(provider->ca7_divider);
	snapshot->mpll_ctrl0 = readl(provider->mpll + UMS9117_MPLL_CTRL0);
	snapshot->mpll_ctrl1 = readl(provider->mpll + UMS9117_MPLL_CTRL1);
	snapshot->mpll_ctrl2 = readl(provider->mpll + UMS9117_MPLL_CTRL2);
}

static bool
ums9117_clock_snapshots_equal(const struct ums9117_clock_snapshot *first,
			      const struct ums9117_clock_snapshot *second)
{
	return first->ca7_source == second->ca7_source &&
	       first->ca7_divider == second->ca7_divider &&
	       first->mpll_ctrl0 == second->mpll_ctrl0 &&
	       first->mpll_ctrl1 == second->mpll_ctrl1 &&
	       first->mpll_ctrl2 == second->mpll_ctrl2;
}

static bool
ums9117_take_clock_snapshot(const struct ums9117_inherited_clks *provider,
			    struct ums9117_clock_snapshot *first,
			    struct ums9117_clock_snapshot *second)
{
	ums9117_read_clock_snapshot(provider, first);
	ums9117_read_clock_snapshot(provider, second);

	return ums9117_clock_snapshots_equal(first, second);
}

static void
ums9117_warn_torn_snapshot(struct ums9117_inherited_clks *provider,
			   const char *clock_name,
			   const struct ums9117_clock_snapshot *first,
			   const struct ums9117_clock_snapshot *second)
{
	dev_warn_ratelimited(
		provider->dev,
		"%s snapshot changed: first=%08x/%08x/%08x/%08x/%08x second=%08x/%08x/%08x/%08x/%08x\n",
		clock_name, first->ca7_source, first->ca7_divider,
		first->mpll_ctrl0, first->mpll_ctrl1, first->mpll_ctrl2,
		second->ca7_source, second->ca7_divider, second->mpll_ctrl0,
		second->mpll_ctrl1, second->mpll_ctrl2);
}

static bool
ums9117_decode_mpll_pre_postdiv(const struct ums9117_clock_snapshot *snapshot,
				u64 *rate)
{
	u32 nint;
	u32 kint;
	u64 whole;
	u64 fractional_numerator;
	u64 fractional;

	if ((snapshot->mpll_ctrl0 &
	     (UMS9117_MPLL_DIV_S | UMS9117_MPLL_SDM_EN)) !=
	    (UMS9117_MPLL_DIV_S | UMS9117_MPLL_SDM_EN))
		return false;

	nint = FIELD_GET(UMS9117_MPLL_NINT_MASK, snapshot->mpll_ctrl1);
	kint = FIELD_GET(UMS9117_MPLL_KINT_MASK, snapshot->mpll_ctrl1);
	if (!nint)
		return false;
	if (check_mul_overflow((u64)nint, UMS9117_XTL_HZ, &whole) ||
	    check_mul_overflow((u64)kint, UMS9117_XTL_HZ,
			       &fractional_numerator))
		return false;

	fractional = DIV_ROUND_CLOSEST_ULL(fractional_numerator,
					   UMS9117_MPLL_KINT_SCALE);
	return !check_add_overflow(whole, fractional, rate);
}

static bool ums9117_decode_mpll(const struct ums9117_clock_snapshot *snapshot,
				unsigned long *rate)
{
	u64 pre_postdiv_rate;

	/* Only the clear POSTDIV state has a verified output relationship. */
	if (snapshot->mpll_ctrl2 & UMS9117_MPLL_POSTDIV)
		return false;
	if (!ums9117_decode_mpll_pre_postdiv(snapshot, &pre_postdiv_rate) ||
	    pre_postdiv_rate > ULONG_MAX)
		return false;

	*rate = pre_postdiv_rate;
	return true;
}

static unsigned long
ums9117_decode_ca7_without_mpll(const struct ums9117_clock_snapshot *snapshot)
{
	unsigned long source_rate;
	u32 source;
	u32 divisor;

	source = FIELD_GET(UMS9117_CA7_SOURCE_MASK, snapshot->ca7_source);
	divisor =
		FIELD_GET(UMS9117_CA7_DIVIDER_MASK, snapshot->ca7_divider) + 1;

	switch (source) {
	case UMS9117_CA7_SOURCE_XTL:
		source_rate = UMS9117_XTL_HZ;
		break;
	case UMS9117_CA7_SOURCE_TWPLL_512M:
		source_rate = UMS9117_TWPLL_512M_HZ;
		break;
	case UMS9117_CA7_SOURCE_TWPLL_768M:
		source_rate = UMS9117_TWPLL_768M_HZ;
		break;
	case UMS9117_CA7_SOURCE_MPLL:
	default:
		return 0;
	}

	return source_rate / divisor;
}

static unsigned long ums9117_mpll_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct ums9117_observed_clk *clock = to_ums9117_observed_clk(hw);
	struct ums9117_inherited_clks *provider = clock->provider;
	struct ums9117_clock_snapshot first;
	struct ums9117_clock_snapshot second;
	unsigned long rate;
	u64 pre_postdiv_rate;

	(void)parent_rate;
	if (!ums9117_take_clock_snapshot(provider, &first, &second)) {
		ums9117_warn_torn_snapshot(provider, "MPLL", &first, &second);
		return 0;
	}

	if (ums9117_decode_mpll(&first, &rate))
		return rate;

	if (!ums9117_decode_mpll_pre_postdiv(&first, &pre_postdiv_rate)) {
		dev_warn_ratelimited(
			provider->dev,
			"MPLL mode is unsupported: ctrl0=%08x ctrl1=%08x ctrl2=%08x\n",
			first.mpll_ctrl0, first.mpll_ctrl1, first.mpll_ctrl2);
		return 0;
	}

	dev_warn_ratelimited(
		provider->dev,
		"MPLL POSTDIV=%u is unsupported (pre-postdiv=%llu Hz)\n",
		!!(first.mpll_ctrl2 & UMS9117_MPLL_POSTDIV), pre_postdiv_rate);

	return 0;
}

static unsigned long ums9117_ca7_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct ums9117_observed_clk *clock = to_ums9117_observed_clk(hw);
	struct ums9117_inherited_clks *provider = clock->provider;
	struct ums9117_clock_snapshot first;
	struct ums9117_clock_snapshot second;
	unsigned long mpll_rate;
	unsigned long rate;
	u64 pre_postdiv_rate;
	u32 source;
	u32 divisor;

	(void)parent_rate;
	if (!ums9117_take_clock_snapshot(provider, &first, &second)) {
		ums9117_warn_torn_snapshot(provider, "CA7", &first, &second);
		return 0;
	}

	rate = ums9117_decode_ca7_without_mpll(&first);
	if (rate)
		return rate;

	source = FIELD_GET(UMS9117_CA7_SOURCE_MASK, first.ca7_source);
	divisor = FIELD_GET(UMS9117_CA7_DIVIDER_MASK, first.ca7_divider) + 1;
	if (source != UMS9117_CA7_SOURCE_MPLL) {
		dev_warn_ratelimited(
			provider->dev,
			"CA7 clock state is unsupported: source=%u divisor=%u ctrl0=%08x ctrl1=%08x ctrl2=%08x\n",
			source, divisor, first.mpll_ctrl0, first.mpll_ctrl1,
			first.mpll_ctrl2);
		return 0;
	}
	if (ums9117_decode_mpll(&first, &mpll_rate))
		return mpll_rate / divisor;
	if (!ums9117_decode_mpll_pre_postdiv(&first, &pre_postdiv_rate)) {
		dev_warn_ratelimited(
			provider->dev,
			"CA7 MPLL mode is unsupported: divisor=%u ctrl0=%08x ctrl1=%08x ctrl2=%08x\n",
			divisor, first.mpll_ctrl0, first.mpll_ctrl1,
			first.mpll_ctrl2);
		return 0;
	}

	dev_warn_ratelimited(
		provider->dev,
		"CA7 MPLL parent has unsupported POSTDIV=%u (divisor=%u pre-postdiv=%llu Hz)\n",
		!!(first.mpll_ctrl2 & UMS9117_MPLL_POSTDIV), divisor,
		pre_postdiv_rate);

	return 0;
}

static int ums9117_ca7_dfs_snapshot(struct ums9117_inherited_clks *provider,
				    struct ums9117_clock_snapshot *snapshot,
				    unsigned long *mpll_rate)
{
	struct ums9117_clock_snapshot second;
	u32 source;

	if (!ums9117_take_clock_snapshot(provider, snapshot, &second))
		return -EBUSY;

	source = FIELD_GET(UMS9117_CA7_SOURCE_MASK, snapshot->ca7_source);
	if ((source != UMS9117_CA7_SOURCE_MPLL &&
	     source != UMS9117_CA7_SOURCE_TWPLL_768M) ||
	    FIELD_GET(UMS9117_CA7_DIVIDER_MASK, snapshot->ca7_divider) ||
	    !ums9117_decode_mpll(snapshot, mpll_rate))
		return -EINVAL;

	/* Fractional PLL quantization puts the inherited 1 GHz one Hz below. */
	if (*mpll_rate != UMS9117_MPLL_1G_HZ - 1 &&
	    *mpll_rate != UMS9117_MPLL_1G_HZ)
		return -EINVAL;

	return 0;
}

static bool ums9117_twpll_ready(struct ums9117_inherited_clks *provider)
{
	u32 debug_mask = UMS9117_ANALOG_TOP_SEL0_TWPLL_REF |
			 UMS9117_ANALOG_TOP_SEL0_TWPLL_PD |
			 UMS9117_ANALOG_TOP_SEL0_TWPLL_RST |
			 UMS9117_ANALOG_TOP_SEL0_TWPLL_DIV2;
	u32 relation_mask = UMS9117_PMU_TWPLL_REL_REF_MASK |
			    UMS9117_PMU_TWPLL_REL_FORCE_OFF |
			    UMS9117_PMU_TWPLL_REL_AP_SEL;
	u32 required_relation = FIELD_PREP(UMS9117_PMU_TWPLL_REL_REF_MASK,
					   UMS9117_PMU_TWPLL_REL_REF_RPLL) |
				UMS9117_PMU_TWPLL_REL_AP_SEL;
	u32 select = readl(provider->analog_top + UMS9117_ANALOG_TOP_SEL0);
	u32 control = readl(provider->analog_top + UMS9117_ANALOG_TOP_CTRL0);
	unsigned int relation;
	int ret;

	ret = regmap_read(provider->pmu, UMS9117_PMU_TWPLL_REL, &relation);
	if (ret) {
		dev_warn_ratelimited(provider->dev,
				     "could not read TWPLL PMU state: %pe\n",
				     ERR_PTR(ret));
		return false;
	}

	/* Without debug overrides, CTRL0 PD/RST/DIV2 are not live state. */
	if (!(select & debug_mask) &&
	    (relation & relation_mask) == required_relation &&
	    (control & UMS9117_ANALOG_TOP_CTRL0_TWPLL_LOCK_DONE))
		return true;

	dev_warn_ratelimited(provider->dev,
			     "TWPLL 768 MHz output is not ready\n");
	dev_dbg(provider->dev,
		"TWPLL state: sel0=%08x sel1=%08x ctrl0=%08x ctrl1=%08x relation=%08x\n",
		select, readl(provider->analog_top + UMS9117_ANALOG_TOP_SEL1),
		control, readl(provider->analog_top + UMS9117_ANALOG_TOP_CTRL1),
		relation);
	return false;
}

static long ums9117_ca7_round_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long *parent_rate)
{
	struct ums9117_inherited_clks *provider =
		to_ums9117_observed_clk(hw)->provider;
	struct ums9117_clock_snapshot snapshot;
	unsigned long mpll_rate;
	int ret;

	ret = ums9117_ca7_dfs_snapshot(provider, &snapshot, &mpll_rate);
	if (ret)
		return ret;

	if (rate >= mpll_rate)
		return mpll_rate;
	if (rate < UMS9117_TWPLL_768M_HZ)
		return -EINVAL;
	if (!ums9117_twpll_ready(provider))
		return -EBUSY;

	return UMS9117_TWPLL_768M_HZ;
}

static int ums9117_ca7_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct ums9117_inherited_clks *provider =
		to_ums9117_observed_clk(hw)->provider;
	struct ums9117_clock_snapshot snapshot;
	unsigned long mpll_rate;
	u32 source;
	u32 value;
	int ret;

	ret = ums9117_ca7_dfs_snapshot(provider, &snapshot, &mpll_rate);
	if (ret)
		return ret;

	if (rate == mpll_rate)
		source = UMS9117_CA7_SOURCE_MPLL;
	else if (rate == UMS9117_TWPLL_768M_HZ)
		source = UMS9117_CA7_SOURCE_TWPLL_768M;
	else
		return -EINVAL;

	/* The default performance policy must leave the boot mux untouched. */
	if (FIELD_GET(UMS9117_CA7_SOURCE_MASK, snapshot.ca7_source) == source)
		return 0;
	if (source == UMS9117_CA7_SOURCE_TWPLL_768M &&
	    !ums9117_twpll_ready(provider))
		return -EBUSY;

	/* CCF serializes rate changes; all PLL and divider state is retained. */
	value = (snapshot.ca7_source & ~UMS9117_CA7_SOURCE_MASK) |
		FIELD_PREP(UMS9117_CA7_SOURCE_MASK, source);
	writel(value, provider->ca7_source);
	if (readl(provider->ca7_source) != value)
		return -EIO;

	return 0;
}

static const struct clk_ops ums9117_mpll_ops = {
	.recalc_rate = ums9117_mpll_recalc_rate,
};

static const struct clk_ops ums9117_ca7_ops = {
	.recalc_rate = ums9117_ca7_recalc_rate,
	.round_rate = ums9117_ca7_round_rate,
	.set_rate = ums9117_ca7_set_rate,
};

static const struct clk_init_data ums9117_mpll_init = {
	.name = "ums9117-mpll",
	.ops = &ums9117_mpll_ops,
	.flags = CLK_GET_RATE_NOCACHE,
};

static const struct clk_init_data ums9117_ca7_init = {
	.name = "ums9117-ca7-core",
	.ops = &ums9117_ca7_ops,
	.flags = CLK_GET_RATE_NOCACHE,
};

static int ums9117_validate_and_map_resource(
	struct platform_device *pdev, unsigned int index, const char *name,
	resource_size_t start, resource_size_t size, void __iomem **base)
{
	struct resource *resource;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, index);
	if (!resource || !resource->name || strcmp(resource->name, name) ||
	    resource->start != start || resource_size(resource) != size)
		return -EINVAL;

	*base = devm_ioremap_resource(&pdev->dev, resource);
	return PTR_ERR_OR_ZERO(*base);
}

static void
ums9117_log_initial_snapshot(struct ums9117_inherited_clks *provider)
{
	struct ums9117_clock_snapshot first;
	struct ums9117_clock_snapshot second;
	unsigned long mpll_rate = 0;
	unsigned long ca7_rate;
	u64 pre_postdiv_rate = 0;
	u32 source;
	u32 divisor;
	bool pre_postdiv_valid;

	if (!ums9117_take_clock_snapshot(provider, &first, &second)) {
		dev_dbg(provider->dev,
			"initial snapshot unstable: first=%08x/%08x/%08x/%08x/%08x second=%08x/%08x/%08x/%08x/%08x\n",
			first.ca7_source, first.ca7_divider, first.mpll_ctrl0,
			first.mpll_ctrl1, first.mpll_ctrl2, second.ca7_source,
			second.ca7_divider, second.mpll_ctrl0,
			second.mpll_ctrl1, second.mpll_ctrl2);
		return;
	}

	source = FIELD_GET(UMS9117_CA7_SOURCE_MASK, first.ca7_source);
	divisor = FIELD_GET(UMS9117_CA7_DIVIDER_MASK, first.ca7_divider) + 1;
	pre_postdiv_valid =
		ums9117_decode_mpll_pre_postdiv(&first, &pre_postdiv_rate);
	ums9117_decode_mpll(&first, &mpll_rate);
	ca7_rate = ums9117_decode_ca7_without_mpll(&first);
	if (!ca7_rate && source == UMS9117_CA7_SOURCE_MPLL && mpll_rate)
		ca7_rate = mpll_rate / divisor;

	dev_dbg(provider->dev,
		"initial snapshot source=%u divisor=%u raw=%08x/%08x/%08x/%08x/%08x pre-postdiv=%llu%s MPLL=%lu%s CA7=%lu%s\n",
		source, divisor, first.ca7_source, first.ca7_divider,
		first.mpll_ctrl0, first.mpll_ctrl1, first.mpll_ctrl2,
		pre_postdiv_rate, pre_postdiv_valid ? " Hz" : " invalid",
		mpll_rate, mpll_rate ? " Hz" : " invalid", ca7_rate,
		ca7_rate ? " Hz" : " invalid");
}

static int ums9117_inherited_clks_probe(struct platform_device *pdev)
{
	struct ums9117_inherited_clks *provider;
	struct device *dev = &pdev->dev;
	int ret;

	provider = devm_kzalloc(dev, sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;
	provider->dev = dev;

	provider->pmu =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,pmu-apb");
	if (IS_ERR(provider->pmu))
		return dev_err_probe(dev, PTR_ERR(provider->pmu),
				     "PMU syscon unavailable\n");

	ret = ums9117_validate_and_map_resource(pdev, 0, "ca7-divider",
						UMS9117_CA7_DIVIDER_PHYS,
						UMS9117_CA7_DIVIDER_BYTES,
						&provider->ca7_divider);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"CA7 divider DT resource does not match UMS9117\n");
	ret = ums9117_validate_and_map_resource(pdev, 1, "ca7-source",
						UMS9117_CA7_SOURCE_PHYS,
						UMS9117_CA7_SOURCE_BYTES,
						&provider->ca7_source);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"CA7 source DT resource does not match UMS9117\n");
	ret = ums9117_validate_and_map_resource(pdev, 2, "mpll",
						UMS9117_MPLL_PHYS,
						UMS9117_MPLL_BYTES,
						&provider->mpll);
	if (ret)
		return dev_err_probe(
			dev, ret, "MPLL DT resource does not match UMS9117\n");
	ret = ums9117_validate_and_map_resource(pdev, 3, "analog-top",
						UMS9117_ANALOG_TOP_PHYS,
						UMS9117_ANALOG_TOP_BYTES,
						&provider->analog_top);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"Analog TOP DT resource does not match UMS9117\n");
	if (platform_get_resource(pdev, IORESOURCE_MEM, 4))
		return dev_err_probe(
			dev, -EINVAL,
			"exactly four DT resources are required\n");

	provider->mpll_clk.provider = provider;
	provider->mpll_clk.hw.init = &ums9117_mpll_init;
	ret = devm_clk_hw_register(dev, &provider->mpll_clk.hw);
	if (ret)
		return dev_err_probe(dev, ret,
				     "could not register inherited MPLL\n");

	provider->ca7_clk.provider = provider;
	provider->ca7_clk.hw.init = &ums9117_ca7_init;
	ret = devm_clk_hw_register(dev, &provider->ca7_clk.hw);
	if (ret)
		return dev_err_probe(
			dev, ret, "could not register inherited CA7 clock\n");

	provider->hw_data = devm_kzalloc(dev,
					 struct_size(provider->hw_data, hws,
						     FPLINUX_UMS9117_CLK_NUM),
					 GFP_KERNEL);
	if (!provider->hw_data)
		return -ENOMEM;
	provider->hw_data->num = FPLINUX_UMS9117_CLK_NUM;
	provider->hw_data->hws[FPLINUX_UMS9117_CLK_MPLL] =
		&provider->mpll_clk.hw;
	provider->hw_data->hws[FPLINUX_UMS9117_CLK_CA7_CORE] =
		&provider->ca7_clk.hw;

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					  provider->hw_data);
	if (ret)
		return dev_err_probe(
			dev, ret, "could not add inherited clock provider\n");

	platform_set_drvdata(pdev, provider);
	ums9117_log_initial_snapshot(provider);

	return 0;
}

static const struct of_device_id ums9117_inherited_clks_of_match[] = {
	{ .compatible = "fplinux,ums9117-inherited-clocks" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_inherited_clks_of_match);

static struct platform_driver ums9117_inherited_clks_driver = {
	.probe = ums9117_inherited_clks_probe,
	.driver = {
		.name = "ums9117-inherited-clocks",
		.of_match_table = ums9117_inherited_clks_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ums9117_inherited_clks_driver);

MODULE_DESCRIPTION("UMS9117 inherited clocks and fixed-voltage CPU scaling");
MODULE_LICENSE("GPL");
