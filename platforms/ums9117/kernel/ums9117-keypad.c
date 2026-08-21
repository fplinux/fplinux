// SPDX-License-Identifier: GPL-2.0-only
/*
 * Polled matrix keypad driver for UMS9117 feature phones.
 *
 * The controller registers, gate/reset recipe and packed scan-code decoding
 * are inherited from the UMS9117 fpdoom implementation.  The controller IRQ
 * route is not qualified, so both matrix and inherited EIC state are polled.
 * In particular, this driver does not initialize the analog EIC controller;
 * it only reads the state left by the RAM bootstrap through the shared ADI
 * provider.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/soc/sprd/ums9117-adi.h>
#include <linux/workqueue.h>

#define UMS9117_KPD_CTRL 0x00
#define UMS9117_KPD_INT_EN 0x04
#define UMS9117_KPD_INT_RAW 0x08
#define UMS9117_KPD_INT_CLR 0x10
#define UMS9117_KPD_POLARITY 0x18
#define UMS9117_KPD_DEBOUNCE 0x1c
#define UMS9117_KPD_CLK_DIVIDE 0x28
#define UMS9117_KPD_KEY_STATUS 0x2c

#define UMS9117_KPD_CTRL_ENABLE BIT(0)
#define UMS9117_KPD_CTRL_SLEEP BIT(1)
#define UMS9117_KPD_CTRL_LONG_KEY BIT(2)
#define UMS9117_KPD_CTRL_MATRIX_LINES_MASK GENMASK(7, 2)
#define UMS9117_KPD_CTRL_ROW_SHIFT 16
#define UMS9117_KPD_CTRL_COL_SHIFT 8

#define UMS9117_AON_APB_PWR_SET 0x1000
#define UMS9117_AON_APB_CLK_SET 0x1010
#define UMS9117_AON_APB_RST_SET 0x1008
#define UMS9117_AON_APB_RST_CLR 0x2008

#define UMS9117_ADI_SLAVE_PHYS 0x40608000U
#define UMS9117_ADI_SLAVE_MMIO_BYTES 0x1000U

#define UMS9117_KPD_MAX_ROWS 8
#define UMS9117_KPD_MAX_COLS 8
#define UMS9117_KPD_POLL_MS 5

#define UMS9117_EIC_DATA_POWER_BIT 1
#define UMS9117_EIC_DATA_EIC9_BIT 9

struct ums9117_keypad {
	struct device *dev;
	void __iomem *kpd;
	struct regmap *aon_apb;
	struct input_dev *input;
	struct delayed_work matrix_poll_work;
	struct delayed_work eic_poll_work;
	u32 eic_data_phys;
	u32 eic9_keycode;
	unsigned int rows;
	unsigned int cols;
	unsigned int row_shift;
	bool has_eic9_key;
	bool eic9_baseline_valid;
	bool eic9_baseline;
	bool eic9_down;
	bool power_down;
	bool eic_error_reported;
	bool stopping;
};

static int ums9117_keypad_adi_read(struct ums9117_keypad *keypad, u16 *value)
{
	struct ums9117_adi_transaction transaction = {};
	u32 offset = keypad->eic_data_phys - UMS9117_ADI_SLAVE_PHYS;
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, offset, value);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	return ret;
}

static void ums9117_keypad_matrix_poll_work(struct work_struct *work)
{
	struct ums9117_keypad *keypad = container_of(
		to_delayed_work(work), struct ums9117_keypad, matrix_poll_work);
	unsigned short *keymap = keypad->input->keycode;
	u32 event = readl(keypad->kpd + UMS9117_KPD_INT_RAW) & 0xff;
	u32 status;
	unsigned int index;
	bool sync = false;

	if (!event)
		goto out;

	status = readl(keypad->kpd + UMS9117_KPD_KEY_STATUS);
	writel(0xfff, keypad->kpd + UMS9117_KPD_INT_CLR);
	if (status & BIT(3))
		goto out;

	for (index = 0; index < 8; ++index) {
		unsigned int packed;
		unsigned int raw_scan;
		unsigned int row;
		unsigned int col;
		unsigned int scan;
		unsigned short code;

		if (!(event & BIT(index)))
			continue;

		packed = status >> ((index & 3) * 8);
		raw_scan = ((packed & 0x70) >> 1) | (packed & 0x07);
		row = raw_scan >> 3;
		col = raw_scan & 0x07;
		if (row >= keypad->rows || col >= keypad->cols)
			continue;

		scan = MATRIX_SCAN_CODE(row, col, keypad->row_shift);
		code = keymap[scan];
		if (!code)
			continue;

		input_event(keypad->input, EV_MSC, MSC_SCAN, scan);
		input_report_key(keypad->input, code, index < 4);
		sync = true;
	}

	if (sync)
		input_sync(keypad->input);
out:
	if (!READ_ONCE(keypad->stopping))
		schedule_delayed_work(&keypad->matrix_poll_work,
				      msecs_to_jiffies(UMS9117_KPD_POLL_MS));
}

static void ums9117_keypad_eic_poll_work(struct work_struct *work)
{
	struct ums9117_keypad *keypad = container_of(
		to_delayed_work(work), struct ums9117_keypad, eic_poll_work);
	bool power_down;
	bool sync = false;
	u16 data;
	int ret;

	ret = ums9117_keypad_adi_read(keypad, &data);
	if (ret) {
		if (!keypad->eic_error_reported) {
			dev_warn(
				keypad->dev,
				"inherited analog EIC read unavailable (%d); matrix keypad remains active\n",
				ret);
			keypad->eic_error_reported = true;
		}
		goto out;
	}

	power_down = !(data & BIT(UMS9117_EIC_DATA_POWER_BIT));
	if (power_down != keypad->power_down) {
		keypad->power_down = power_down;
		input_report_key(keypad->input, KEY_POWER, power_down);
		sync = true;
	}

	if (keypad->has_eic9_key) {
		bool level = !!(data & BIT(UMS9117_EIC_DATA_EIC9_BIT));

		if (!keypad->eic9_baseline_valid) {
			/* Preserve the inherited EIC9 polarity and suppress boot noise. */
			keypad->eic9_baseline = level;
			keypad->eic9_baseline_valid = true;
		} else if ((level != keypad->eic9_baseline) !=
			   keypad->eic9_down) {
			keypad->eic9_down = level != keypad->eic9_baseline;
			input_report_key(keypad->input, keypad->eic9_keycode,
					 keypad->eic9_down);
			sync = true;
		}
	}

	if (sync)
		input_sync(keypad->input);
out:
	if (!READ_ONCE(keypad->stopping))
		schedule_delayed_work(&keypad->eic_poll_work,
				      msecs_to_jiffies(UMS9117_KPD_POLL_MS));
}

static int ums9117_keypad_matrix_masks(struct ums9117_keypad *keypad,
				       u8 *row_mask, u8 *col_mask)
{
	unsigned short *keymap = keypad->input->keycode;
	u8 rows = 0;
	u8 cols = 0;
	unsigned int row;
	unsigned int col;

	for (row = 0; row < keypad->rows; ++row)
		for (col = 0; col < keypad->cols; ++col)
			if (keymap[MATRIX_SCAN_CODE(row, col,
						    keypad->row_shift)]) {
				rows |= BIT(row);
				cols |= BIT(col);
			}

	/* The controller reserves row and column lines 0 and 1. */
	rows &= UMS9117_KPD_CTRL_MATRIX_LINES_MASK;
	cols &= UMS9117_KPD_CTRL_MATRIX_LINES_MASK;
	if (!rows || !cols)
		return -EINVAL;

	*row_mask = rows;
	*col_mask = cols;
	return 0;
}

static int ums9117_keypad_hw_init(struct ums9117_keypad *keypad)
{
	u8 row_mask;
	u8 col_mask;
	u32 ctrl;
	int ret;

	ret = ums9117_keypad_matrix_masks(keypad, &row_mask, &col_mask);
	if (ret)
		return dev_err_probe(
			keypad->dev, ret,
			"keymap does not select usable controller lines\n");

	/* Exact UMS9117 gate/reset sequence used by fpdoom. */
	ret = regmap_write(keypad->aon_apb, UMS9117_AON_APB_PWR_SET, 0x100);
	if (ret)
		return ret;
	ret = regmap_write(keypad->aon_apb, UMS9117_AON_APB_CLK_SET, 0x002);
	if (ret)
		return ret;
	ret = regmap_write(keypad->aon_apb, UMS9117_AON_APB_RST_SET, 0x100);
	if (ret)
		return ret;
	udelay(100);
	ret = regmap_write(keypad->aon_apb, UMS9117_AON_APB_RST_CLR, 0x100);
	if (ret)
		return ret;
	udelay(100);

	writel(0xfff, keypad->kpd + UMS9117_KPD_INT_CLR);
	writel(1, keypad->kpd + UMS9117_KPD_CLK_DIVIDE);
	writel(16, keypad->kpd + UMS9117_KPD_DEBOUNCE);
	/* This enables controller event latches; the driver does not request an IRQ. */
	writel(0xfff, keypad->kpd + UMS9117_KPD_INT_EN);
	writel(0xffff, keypad->kpd + UMS9117_KPD_POLARITY);

	ctrl = readl(keypad->kpd + UMS9117_KPD_CTRL);
	ctrl |= UMS9117_KPD_CTRL_ENABLE | UMS9117_KPD_CTRL_LONG_KEY;
	ctrl &= ~UMS9117_KPD_CTRL_SLEEP;
	ctrl &= ~((u32)UMS9117_KPD_CTRL_MATRIX_LINES_MASK
			  << UMS9117_KPD_CTRL_ROW_SHIFT |
		  (u32)UMS9117_KPD_CTRL_MATRIX_LINES_MASK
			  << UMS9117_KPD_CTRL_COL_SHIFT);
	ctrl |= (u32)row_mask << UMS9117_KPD_CTRL_ROW_SHIFT;
	ctrl |= (u32)col_mask << UMS9117_KPD_CTRL_COL_SHIFT;
	writel(ctrl, keypad->kpd + UMS9117_KPD_CTRL);

	return 0;
}

static int ums9117_keypad_parse_eic(struct ums9117_keypad *keypad)
{
	struct device_node *np = keypad->dev->of_node;
	int ret;

	ret = of_property_read_u32(np, "sprd,eic-data-address",
				   &keypad->eic_data_phys);
	if (ret)
		return dev_err_probe(
			keypad->dev, ret,
			"missing inherited analog EIC data address\n");
	if (!IS_ALIGNED(keypad->eic_data_phys, sizeof(u32)) ||
	    keypad->eic_data_phys < UMS9117_ADI_SLAVE_PHYS ||
	    keypad->eic_data_phys > UMS9117_ADI_SLAVE_PHYS +
					    UMS9117_ADI_SLAVE_MMIO_BYTES -
					    sizeof(u32))
		return dev_err_probe(
			keypad->dev, -EINVAL,
			"invalid inherited analog EIC data address\n");

	ret = of_property_read_u32(np, "sprd,eic9-keycode",
				   &keypad->eic9_keycode);
	if (ret == -EINVAL)
		return 0;
	if (ret)
		return dev_err_probe(keypad->dev, ret,
				     "invalid EIC9 keycode\n");
	if (keypad->eic9_keycode > KEY_MAX)
		return dev_err_probe(keypad->dev, -EINVAL,
				     "EIC9 keycode is outside the input ABI\n");

	keypad->has_eic9_key = true;
	return 0;
}

static int ums9117_keypad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ums9117_keypad *keypad;
	struct input_dev *input;
	const char *name;
	int ret;

	keypad = devm_kzalloc(dev, sizeof(*keypad), GFP_KERNEL);
	if (!keypad)
		return -ENOMEM;
	keypad->dev = dev;

	keypad->kpd = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(keypad->kpd))
		return PTR_ERR(keypad->kpd);
	keypad->aon_apb =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,aon-apb");
	if (IS_ERR(keypad->aon_apb))
		return dev_err_probe(dev, PTR_ERR(keypad->aon_apb),
				     "could not resolve AON APB syscon\n");

	ret = matrix_keypad_parse_properties(dev, &keypad->rows, &keypad->cols);
	if (ret)
		return ret;
	if (keypad->rows > UMS9117_KPD_MAX_ROWS ||
	    keypad->cols > UMS9117_KPD_MAX_COLS)
		return dev_err_probe(
			dev, -EINVAL,
			"matrix exceeds the 8 by 8 UMS9117 controller\n");
	keypad->row_shift = get_count_order(keypad->cols);

	ret = ums9117_keypad_parse_eic(keypad);
	if (ret)
		return ret;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	keypad->input = input;
	input->id.bustype = BUS_HOST;
	ret = device_property_read_string(dev, "label", &name);
	if (ret)
		return dev_err_probe(dev, ret, "missing input label\n");
	input->name = name;
	input->phys = "fplinux/keypad0";

	ret = matrix_keypad_build_keymap(NULL, NULL, keypad->rows, keypad->cols,
					 NULL, input);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to build matrix keymap\n");
	input_set_capability(input, EV_MSC, MSC_SCAN);
	input_set_capability(input, EV_KEY, KEY_POWER);
	if (keypad->has_eic9_key)
		input_set_capability(input, EV_KEY, keypad->eic9_keycode);

	ret = ums9117_keypad_hw_init(keypad);
	if (ret)
		return ret;
	ret = input_register_device(input);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&keypad->matrix_poll_work,
			  ums9117_keypad_matrix_poll_work);
	INIT_DELAYED_WORK(&keypad->eic_poll_work, ums9117_keypad_eic_poll_work);
	platform_set_drvdata(pdev, keypad);
	schedule_delayed_work(&keypad->matrix_poll_work,
			      msecs_to_jiffies(UMS9117_KPD_POLL_MS));
	schedule_delayed_work(&keypad->eic_poll_work,
			      msecs_to_jiffies(UMS9117_KPD_POLL_MS));

	dev_info(dev, "polled UMS9117 keypad registered (EIC1 power%s)\n",
		 keypad->has_eic9_key ? " and EIC9 key" : "");
	return 0;
}

static void ums9117_keypad_stop(struct ums9117_keypad *keypad)
{
	WRITE_ONCE(keypad->stopping, true);
	cancel_delayed_work_sync(&keypad->matrix_poll_work);
	cancel_delayed_work_sync(&keypad->eic_poll_work);
}

static void ums9117_keypad_remove(struct platform_device *pdev)
{
	ums9117_keypad_stop(platform_get_drvdata(pdev));
}

static void ums9117_keypad_shutdown(struct platform_device *pdev)
{
	ums9117_keypad_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id ums9117_keypad_of_match[] = {
	{ .compatible = "fplinux,ums9117-keypad" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_keypad_of_match);

static struct platform_driver ums9117_keypad_driver = {
	.probe = ums9117_keypad_probe,
	.remove = ums9117_keypad_remove,
	.shutdown = ums9117_keypad_shutdown,
	.driver = {
		.name = "ums9117-keypad",
		.of_match_table = ums9117_keypad_of_match,
	},
};
module_platform_driver(ums9117_keypad_driver);

MODULE_DESCRIPTION("Polled UMS9117 matrix and inherited EIC keypad");
MODULE_LICENSE("GPL");
