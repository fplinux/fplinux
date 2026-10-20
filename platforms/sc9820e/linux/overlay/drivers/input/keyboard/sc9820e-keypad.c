// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define SC9820E_KPD_CTRL 0x00
#define SC9820E_KPD_INT_EN 0x04
#define SC9820E_KPD_INT_MASK_STATUS 0x0c
#define SC9820E_KPD_INT_CLR 0x10
#define SC9820E_KPD_POLARITY 0x18
#define SC9820E_KPD_DEBOUNCE_CNT 0x1c
#define SC9820E_KPD_LONG_KEY_CNT 0x20
#define SC9820E_KPD_SLEEP_CNT 0x24
#define SC9820E_KPD_CLK_DIV_CNT 0x28
#define SC9820E_KPD_KEY_STATUS 0x2c

#define SC9820E_KPD_CTRL_EN BIT(0)
#define SC9820E_KPD_CTRL_SLEEP_EN BIT(1)
#define SC9820E_KPD_INT_ALL GENMASK(11, 0)
#define SC9820E_KPD_INT_DOWN_UP GENMASK(7, 0)
#define SC9820E_KPD_MAX_ROWS 8
#define SC9820E_KPD_MAX_COLS 8
#define SC9820E_KPD_MAX_KEYS 64
#define SC9820E_KPD_SLOT_COUNT 4

#define SC9820E_AON_APB_EB0 0x00
#define SC9820E_AON_APB_RST0 0x08
#define SC9820E_AON_APB_RTC_EB 0x10
#define SC9820E_AON_APB_SET 0x1000
#define SC9820E_AON_APB_CLR 0x2000
#define SC9820E_AON_APB_KPD_EB BIT(8)
#define SC9820E_AON_APB_KPD_SOFT_RST BIT(8)
#define SC9820E_AON_APB_KPD_RTC_EB BIT(1)

struct sc9820e_keypad {
	struct device *dev;
	struct input_dev *input;
	struct regmap *aon;
	void __iomem *base;
	unsigned int rows;
	unsigned int cols;
	unsigned int row_shift;
	u32 rows_mask;
	u32 cols_mask;
	u32 debounce_count;
	int irq;
	bool enabled;
};

static void disable_gates(struct sc9820e_keypad *keypad)
{
	int error;

	error = regmap_write(keypad->aon,
			     SC9820E_AON_APB_RTC_EB + SC9820E_AON_APB_CLR,
			     SC9820E_AON_APB_KPD_RTC_EB);
	if (error)
		dev_err(keypad->dev, "failed to disable RTC gate: %d\n", error);

	error = regmap_write(keypad->aon,
			     SC9820E_AON_APB_EB0 + SC9820E_AON_APB_CLR,
			     SC9820E_AON_APB_KPD_EB);
	if (error)
		dev_err(keypad->dev, "failed to disable APB gate: %d\n", error);
}

static int enable_gates(struct sc9820e_keypad *keypad)
{
	int error;
	int reset_error;

	error = regmap_write(keypad->aon,
			     SC9820E_AON_APB_RST0 + SC9820E_AON_APB_SET,
			     SC9820E_AON_APB_KPD_SOFT_RST);
	if (error)
		return error;
	udelay(5);

	error = regmap_write(keypad->aon,
			     SC9820E_AON_APB_RST0 + SC9820E_AON_APB_CLR,
			     SC9820E_AON_APB_KPD_SOFT_RST);
	if (error)
		goto disable;
	udelay(5);

	error = regmap_write(keypad->aon,
			     SC9820E_AON_APB_RTC_EB + SC9820E_AON_APB_SET,
			     SC9820E_AON_APB_KPD_RTC_EB);
	if (error)
		goto disable;
	udelay(5);

	error = regmap_write(keypad->aon,
			     SC9820E_AON_APB_EB0 + SC9820E_AON_APB_SET,
			     SC9820E_AON_APB_KPD_EB);
	if (error)
		goto disable;

	keypad->enabled = true;
	return 0;

disable:
	reset_error = regmap_write(keypad->aon,
				   SC9820E_AON_APB_RST0 + SC9820E_AON_APB_CLR,
				   SC9820E_AON_APB_KPD_SOFT_RST);
	if (reset_error)
		dev_err(keypad->dev, "failed to release reset: %d\n",
			reset_error);
	disable_gates(keypad);
	return error;
}

static void stop_controller(void *data)
{
	struct sc9820e_keypad *keypad = data;
	u32 control;

	if (!keypad->enabled)
		return;

	writel(0, keypad->base + SC9820E_KPD_INT_EN);
	control = readl(keypad->base + SC9820E_KPD_CTRL);
	control &= ~(SC9820E_KPD_CTRL_EN | SC9820E_KPD_CTRL_SLEEP_EN);
	writel(control, keypad->base + SC9820E_KPD_CTRL);
	writel(SC9820E_KPD_INT_ALL, keypad->base + SC9820E_KPD_INT_CLR);
	readl(keypad->base + SC9820E_KPD_CTRL);
	disable_gates(keypad);
	keypad->enabled = false;
}

static int read_keymap(struct sc9820e_keypad *keypad)
{
	struct matrix_keymap_data keymap_data;
	DECLARE_BITMAP(mapped, SC9820E_KPD_MAX_KEYS);
	u32 keys[SC9820E_KPD_MAX_KEYS];
	unsigned int row, col, code, scan;
	int count, error, i;

	count = device_property_count_u32(keypad->dev, "linux,keymap");
	if (count <= 0 || count > keypad->rows * keypad->cols)
		return -EINVAL;

	error = device_property_read_u32_array(keypad->dev, "linux,keymap",
					       keys, count);
	if (error)
		return error;

	bitmap_zero(mapped, SC9820E_KPD_MAX_KEYS);
	for (i = 0; i < count; i++) {
		row = KEY_ROW(keys[i]);
		col = KEY_COL(keys[i]);
		code = KEY_VAL(keys[i]);
		if (row >= keypad->rows || col >= keypad->cols ||
		    code == KEY_RESERVED || code > KEY_MAX)
			return -EINVAL;

		scan = MATRIX_SCAN_CODE(row, col, keypad->row_shift);
		if (test_and_set_bit(scan, mapped))
			return -EINVAL;
	}

	keymap_data.keymap = keys;
	keymap_data.keymap_size = count;
	return matrix_keypad_build_keymap(&keymap_data, NULL, keypad->rows,
					  keypad->cols, NULL, keypad->input);
}

static int read_properties(struct sc9820e_keypad *keypad)
{
	int error;

	error = matrix_keypad_parse_properties(keypad->dev, &keypad->rows,
					       &keypad->cols);
	if (error)
		return error;
	if (keypad->rows > SC9820E_KPD_MAX_ROWS ||
	    keypad->cols > SC9820E_KPD_MAX_COLS)
		return -EINVAL;
	keypad->row_shift = get_count_order(keypad->cols);

	/* Scan coordinates directly address contiguous enabled rows and columns. */
	keypad->rows_mask = GENMASK(keypad->rows - 1, 0) << 16;
	keypad->cols_mask = GENMASK(keypad->cols - 1, 0) << 8;

	return device_property_read_u32(keypad->dev, "fplinux,debounce-count",
					&keypad->debounce_count);
}

static irqreturn_t keypad_irq(int irq, void *data)
{
	struct sc9820e_keypad *keypad = data;
	unsigned short *keycodes = keypad->input->keycode;
	u32 status, keys;
	unsigned int slot, row, col, scan, code;

	status = readl(keypad->base + SC9820E_KPD_INT_MASK_STATUS);
	status &= SC9820E_KPD_INT_ALL;
	if (!status)
		return IRQ_NONE;
	keys = readl(keypad->base + SC9820E_KPD_KEY_STATUS);
	writel(SC9820E_KPD_INT_ALL, keypad->base + SC9820E_KPD_INT_CLR);

	for (slot = 0; slot < SC9820E_KPD_SLOT_COUNT; slot++) {
		if (!(status & (BIT(slot) | BIT(slot + 4))))
			continue;
		col = (keys >> (slot * 8)) & 0x7;
		row = (keys >> (slot * 8 + 4)) & 0x7;
		if (row >= keypad->rows || col >= keypad->cols)
			continue;
		scan = MATRIX_SCAN_CODE(row, col, keypad->row_shift);
		code = keycodes[scan];
		if (code == KEY_RESERVED || code > KEY_MAX)
			continue;

		if (status & BIT(slot)) {
			input_report_key(keypad->input, code, 1);
			input_sync(keypad->input);
		}
		if (status & BIT(slot + 4)) {
			input_report_key(keypad->input, code, 0);
			input_sync(keypad->input);
		}
	}

	return IRQ_HANDLED;
}

static int keypad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sc9820e_keypad *keypad;
	int error;

	keypad = devm_kzalloc(dev, sizeof(*keypad), GFP_KERNEL);
	if (!keypad)
		return -ENOMEM;
	keypad->dev = dev;

	keypad->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(keypad->base))
		return PTR_ERR(keypad->base);
	keypad->aon = syscon_regmap_lookup_by_phandle(dev->of_node,
						      "fplinux,aon-syscon");
	if (IS_ERR(keypad->aon))
		return dev_err_probe(dev, PTR_ERR(keypad->aon),
				     "failed to get AON regmap\n");
	keypad->irq = platform_get_irq(pdev, 0);
	if (keypad->irq < 0)
		return keypad->irq;

	error = read_properties(keypad);
	if (error)
		return dev_err_probe(dev, error, "invalid matrix properties\n");

	keypad->input = devm_input_allocate_device(dev);
	if (!keypad->input)
		return -ENOMEM;
	keypad->input->name = "sc9820e-keypad";
	keypad->input->phys = "fplinux/keypad0";
	keypad->input->id.bustype = BUS_HOST;
	error = read_keymap(keypad);
	if (error)
		return dev_err_probe(dev, error, "invalid matrix keymap\n");

	error = enable_gates(keypad);
	if (error)
		return dev_err_probe(dev, error,
				     "failed to enable controller\n");
	error = devm_add_action_or_reset(dev, stop_controller, keypad);
	if (error)
		return error;

	writel(0, keypad->base + SC9820E_KPD_INT_EN);
	writel(0, keypad->base + SC9820E_KPD_CTRL);
	writel(SC9820E_KPD_INT_ALL, keypad->base + SC9820E_KPD_INT_CLR);
	writel(0xffff, keypad->base + SC9820E_KPD_POLARITY);
	writel(1, keypad->base + SC9820E_KPD_CLK_DIV_CNT);
	writel(0xc, keypad->base + SC9820E_KPD_LONG_KEY_CNT);
	writel(keypad->debounce_count, keypad->base + SC9820E_KPD_DEBOUNCE_CNT);
	writel(0x7fff, keypad->base + SC9820E_KPD_SLEEP_CNT);

	/* Acquire the IRQ last so devres stops handlers before input teardown. */
	error = input_register_device(keypad->input);
	if (error)
		return dev_err_probe(dev, error, "failed to register input\n");
	error = devm_request_irq(dev, keypad->irq, keypad_irq, 0, dev_name(dev),
				 keypad);
	if (error)
		return dev_err_probe(dev, error, "failed to request IRQ\n");

	platform_set_drvdata(pdev, keypad);
	writel(SC9820E_KPD_INT_DOWN_UP, keypad->base + SC9820E_KPD_INT_EN);
	writel(keypad->rows_mask | keypad->cols_mask | SC9820E_KPD_CTRL_EN |
		       SC9820E_KPD_CTRL_SLEEP_EN,
	       keypad->base + SC9820E_KPD_CTRL);
	return 0;
}

static void keypad_shutdown(struct platform_device *pdev)
{
	struct sc9820e_keypad *keypad = platform_get_drvdata(pdev);

	disable_irq(keypad->irq);
	stop_controller(keypad);
}

static const struct of_device_id keypad_of_match[] = {
	{ .compatible = "fplinux,sc9820e-keypad" },
	{}
};
MODULE_DEVICE_TABLE(of, keypad_of_match);

static struct platform_driver keypad_driver = {
	.probe = keypad_probe,
	.shutdown = keypad_shutdown,
	.driver = {
		.name = "sc9820e-keypad",
		.of_match_table = keypad_of_match,
	},
};
module_platform_driver(keypad_driver);

MODULE_DESCRIPTION("SC9820E v1 matrix keypad driver");
MODULE_LICENSE("GPL");
