// SPDX-License-Identifier: GPL-2.0-only

#include <dt-bindings/reset/sprd,ums9117-reset.h>
#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define UMS9117_AP_AHB_RESET_STATE 0x0004
#define UMS9117_AP_AHB_RESET_SET 0x1004
#define UMS9117_AP_AHB_RESET_CLEAR 0x2004

/*
 * UMS9117 glb/ap_ahb.h defines SDIO0_SOFT_RST as AHB_RST bit 11. The
 * global-register contract uses write-one SET and CLEAR aliases at +0x1000
 * and +0x2000, so reset updates never read-modify-write the shared state.
 */
struct ums9117_reset {
	struct regmap *regmap;
	struct reset_controller_dev rcdev;
};

static const u32 ums9117_ap_ahb_reset_bits[] = {
	[RESET_SDIO0] = BIT(11),
};

static struct ums9117_reset *
to_ums9117_reset(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct ums9117_reset, rcdev);
}

static int ums9117_reset_update(struct reset_controller_dev *rcdev,
				unsigned long id, bool assert)
{
	struct ums9117_reset *reset = to_ums9117_reset(rcdev);
	u32 offset = assert ? UMS9117_AP_AHB_RESET_SET :
			      UMS9117_AP_AHB_RESET_CLEAR;

	if (id >= ARRAY_SIZE(ums9117_ap_ahb_reset_bits))
		return -EINVAL;

	return regmap_write(reset->regmap, offset,
			    ums9117_ap_ahb_reset_bits[id]);
}

static int ums9117_reset_assert(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	return ums9117_reset_update(rcdev, id, true);
}

static int ums9117_reset_deassert(struct reset_controller_dev *rcdev,
				  unsigned long id)
{
	return ums9117_reset_update(rcdev, id, false);
}

static int ums9117_reset_status(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	struct ums9117_reset *reset = to_ums9117_reset(rcdev);
	unsigned int value;
	int ret;

	if (id >= ARRAY_SIZE(ums9117_ap_ahb_reset_bits))
		return -EINVAL;
	ret = regmap_read(reset->regmap, UMS9117_AP_AHB_RESET_STATE, &value);
	if (ret)
		return ret;

	return !!(value & ums9117_ap_ahb_reset_bits[id]);
}

static int ums9117_reset_reset(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	int status;
	int cleanup_ret;
	int ret;

	ret = ums9117_reset_assert(rcdev, id);
	if (ret)
		return ret;
	status = ums9117_reset_status(rcdev, id);
	if (status != 1) {
		cleanup_ret = ums9117_reset_deassert(rcdev, id);
		if (status >= 0 && cleanup_ret)
			return cleanup_ret;
		return status < 0 ? status : -EIO;
	}

	ret = ums9117_reset_deassert(rcdev, id);
	if (ret)
		return ret;
	status = ums9117_reset_status(rcdev, id);
	if (status < 0)
		return status;

	return status ? -EIO : 0;
}

static const struct reset_control_ops ums9117_reset_ops = {
	.assert = ums9117_reset_assert,
	.deassert = ums9117_reset_deassert,
	.reset = ums9117_reset_reset,
	.status = ums9117_reset_status,
};

static int ums9117_reset_probe(struct platform_device *pdev)
{
	struct ums9117_reset *reset;

	reset = devm_kzalloc(&pdev->dev, sizeof(*reset), GFP_KERNEL);
	if (!reset)
		return -ENOMEM;
	reset->regmap = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							"sprd,syscon");
	if (IS_ERR(reset->regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(reset->regmap),
				     "failed to get AP AHB syscon\n");

	reset->rcdev.owner = THIS_MODULE;
	reset->rcdev.nr_resets = RESET_AP_AHB_NUM;
	reset->rcdev.ops = &ums9117_reset_ops;
	reset->rcdev.of_node = pdev->dev.of_node;
	reset->rcdev.dev = &pdev->dev;

	return devm_reset_controller_register(&pdev->dev, &reset->rcdev);
}

static const struct of_device_id ums9117_reset_of_match[] = {
	{ .compatible = "sprd,ums9117-apahb-reset" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_reset_of_match);

static struct platform_driver ums9117_reset_driver = {
	.probe = ums9117_reset_probe,
	.driver = {
		.name = "ums9117-reset",
		.of_match_table = ums9117_reset_of_match,
	},
};
module_platform_driver(ums9117_reset_driver);

MODULE_DESCRIPTION("Unisoc UMS9117 reset controller");
MODULE_LICENSE("GPL v2");
