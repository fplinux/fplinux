// SPDX-License-Identifier: GPL-2.0-only
#include <linux/device.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "sc9820e-gate.h"

#define SC9820E_GATE_SET 0x1000
#define SC9820E_GATE_CLEAR 0x2000

static struct sc9820e_gate *to_gate(struct clk_hw *hw)
{
	return container_of(hw, struct sc9820e_gate, hw);
}

static int sc9820e_gate_enable(struct clk_hw *hw)
{
	struct sc9820e_gate *gate = to_gate(hw);

	return regmap_write(gate->regmap, gate->offset + SC9820E_GATE_SET,
			    gate->mask);
}

static void sc9820e_gate_disable(struct clk_hw *hw)
{
	struct sc9820e_gate *gate = to_gate(hw);
	int ret;

	ret = regmap_write(gate->regmap, gate->offset + SC9820E_GATE_CLEAR,
			   gate->mask);
	if (ret)
		dev_err(gate->dev, "failed to disable %s clock: %d\n",
			gate->name, ret);
}

static int sc9820e_gate_is_enabled(struct clk_hw *hw)
{
	struct sc9820e_gate *gate = to_gate(hw);
	unsigned int value;
	int ret;

	ret = regmap_read(gate->regmap, gate->offset, &value);
	if (ret)
		return ret;

	return !!(value & gate->mask);
}

const struct clk_ops sc9820e_gate_ops = {
	.enable = sc9820e_gate_enable,
	.disable = sc9820e_gate_disable,
	.is_enabled = sc9820e_gate_is_enabled,
};
EXPORT_SYMBOL_GPL(sc9820e_gate_ops);

MODULE_DESCRIPTION("SC9820E atomic set and clear clock gates");
MODULE_LICENSE("GPL");
