/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SC9820E_GATE_H
#define FPLINUX_SC9820E_GATE_H

#include <linux/clk-provider.h>
#include <linux/types.h>

struct device;
struct regmap;

struct sc9820e_gate {
	struct clk_hw hw;
	struct regmap *regmap;
	struct device *dev;
	const char *name;
	u32 offset;
	u32 mask;
};

extern const struct clk_ops sc9820e_gate_ops;

#endif
