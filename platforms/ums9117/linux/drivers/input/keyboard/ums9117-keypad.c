// SPDX-License-Identifier: GPL-2.0-only
/*
 * Matrix keypad driver for UMS9117 feature phones.
 *
 * The controller registers, gate/reset recipe and packed scan-code decoding
 * are inherited from the UMS9117 fpdoom implementation. The controller
 * interrupt reports matrix key changes. Target-owned analog EIC keys are optional GPIO inputs supplied by the SC2720 EIC provider.
 * This driver consumes their events; it does not configure the analog EIC
 * controller directly.
 */
#include <linux/err.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/ums9117-keypad.h>
#include <linux/input/matrix_keypad.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/regmap.h>

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
#define UMS9117_KPD_INT_EVENT_MASK GENMASK(7, 0)
#define UMS9117_KPD_INT_OWNED_MASK GENMASK(11, 0)

#define UMS9117_AON_APB_PWR_SET 0x1000
#define UMS9117_AON_APB_CLK_SET 0x1010
#define UMS9117_AON_APB_RST_SET 0x1008
#define UMS9117_AON_APB_RST_CLR 0x2008

#define UMS9117_KPD_MAX_ROWS 8
#define UMS9117_KPD_MAX_COLS 8
/*
 * EIC keys share the keycode table with the matrix so that the standard evdev
 * keymap ioctls can read and rewrite every key. Their scancodes follow the
 * largest possible matrix and therefore do not depend on the board's size.
 */
#define UMS9117_KPD_MATRIX_KEYS (UMS9117_KPD_MAX_ROWS * UMS9117_KPD_MAX_COLS)
#define UMS9117_KPD_EIC1_SCANCODE UMS9117_KPD_MATRIX_KEYS
#define UMS9117_KPD_EIC9_SCANCODE (UMS9117_KPD_MATRIX_KEYS + 1)
#define UMS9117_KPD_KEYMAP_KEYS (UMS9117_KPD_MATRIX_KEYS + 2)
#define UMS9117_GIC_SPI_HWIRQ_BASE 32
#define UMS9117_KPD_MATRIX_IRQ_SPI 36
#define UMS9117_KPD_MATRIX_IRQ_HWIRQ \
	(UMS9117_GIC_SPI_HWIRQ_BASE + UMS9117_KPD_MATRIX_IRQ_SPI)

struct ums9117_keypad;

struct ums9117_keypad_eic_key {
	struct ums9117_keypad *keypad;
	struct gpio_desc *gpiod;
	unsigned int scancode;
	int irq;
	bool down;
	bool irq_enabled;
	bool suspend_disabled;
	bool wake_enabled;
};

struct ums9117_keypad {
	struct device *dev;
	void __iomem *kpd;
	struct regmap *aon_apb;
	struct input_dev *input;
	int matrix_irq;
	struct ums9117_keypad_eic_key eic1;
	struct ums9117_keypad_eic_key eic9;
	unsigned short keymap[UMS9117_KPD_KEYMAP_KEYS];
	unsigned int rows;
	unsigned int cols;
	unsigned int row_shift;
	bool stopping;
};

static BLOCKING_NOTIFIER_HEAD(ums9117_keypad_power_notifier);

int ums9117_keypad_register_power_notifier(struct notifier_block *notifier)
{
	return blocking_notifier_chain_register(&ums9117_keypad_power_notifier,
						notifier);
}

int ums9117_keypad_unregister_power_notifier(struct notifier_block *notifier)
{
	return blocking_notifier_chain_unregister(
		&ums9117_keypad_power_notifier, notifier);
}

static void ums9117_keypad_notify_power(struct ums9117_keypad *keypad,
					bool down)
{
	struct ums9117_keypad_power_event event = {
		.keypad_node = keypad->dev->of_node,
	};

	blocking_notifier_call_chain(&ums9117_keypad_power_notifier,
				     down ? UMS9117_KEYPAD_POWER_PRESS :
					    UMS9117_KEYPAD_POWER_RELEASE,
				     &event);
}

static void ums9117_keypad_report_matrix(struct ums9117_keypad *keypad,
					 u32 event, u32 status)
{
	unsigned short *keymap = keypad->input->keycode;
	unsigned int index;
	bool sync = false;

	if (status & BIT(3))
		return;

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
}

static void ums9117_keypad_mask_matrix_irq(struct ums9117_keypad *keypad)
{
	u32 ctrl;

	writel(0, keypad->kpd + UMS9117_KPD_INT_EN);
	ctrl = readl(keypad->kpd + UMS9117_KPD_CTRL);
	ctrl &= ~UMS9117_KPD_CTRL_ENABLE;
	writel(ctrl, keypad->kpd + UMS9117_KPD_CTRL);
	writel(UMS9117_KPD_INT_OWNED_MASK, keypad->kpd + UMS9117_KPD_INT_CLR);
}

static irqreturn_t ums9117_keypad_matrix_irq(int irq, void *data)
{
	struct ums9117_keypad *keypad = data;
	u32 raw;
	u32 status;

	if (READ_ONCE(keypad->stopping))
		return IRQ_HANDLED;

	raw = readl(keypad->kpd + UMS9117_KPD_INT_RAW) &
	      UMS9117_KPD_INT_OWNED_MASK;
	if (!raw) {
		ums9117_keypad_mask_matrix_irq(keypad);
		dev_err(keypad->dev,
			"matrix IRQ arrived without owned status; controller masked\n");
		return IRQ_HANDLED;
	}

	/* Bits 8 through 11 are long-key latches and have no scan-code payload. */
	status = readl(keypad->kpd + UMS9117_KPD_KEY_STATUS);
	writel(raw, keypad->kpd + UMS9117_KPD_INT_CLR);
	ums9117_keypad_report_matrix(keypad, raw & UMS9117_KPD_INT_EVENT_MASK,
				     status);

	return IRQ_HANDLED;
}

static int ums9117_keypad_sample_eic_key(struct ums9117_keypad_eic_key *key,
					 bool report)
{
	struct ums9117_keypad *keypad = key->keypad;
	int value;

	if (!key->gpiod)
		return 0;

	value = gpiod_get_value_cansleep(key->gpiod);
	if (value < 0)
		return value;
	if (!!value == key->down)
		return 0;

	key->down = !!value;
	if (key == &keypad->eic1)
		ums9117_keypad_notify_power(keypad, key->down);
	if (report) {
		input_event(keypad->input, EV_MSC, MSC_SCAN, key->scancode);
		input_report_key(keypad->input, keypad->keymap[key->scancode],
				 key->down);
	}

	return report;
}

static irqreturn_t ums9117_keypad_eic_irq_thread(int irq, void *data)
{
	struct ums9117_keypad_eic_key *key = data;
	int ret;

	if (READ_ONCE(key->keypad->stopping))
		return IRQ_HANDLED;

	if (key == &key->keypad->eic1 && device_may_wakeup(key->keypad->dev))
		pm_wakeup_event(key->keypad->dev, 0);

	ret = ums9117_keypad_sample_eic_key(key, true);
	if (ret < 0)
		dev_warn_ratelimited(
			key->keypad->dev,
			"EIC GPIO read failed for scancode %u: %pe\n",
			key->scancode, ERR_PTR(ret));
	else if (ret)
		input_sync(key->keypad->input);

	return IRQ_HANDLED;
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

	writel(UMS9117_KPD_INT_OWNED_MASK, keypad->kpd + UMS9117_KPD_INT_CLR);
	writel(1, keypad->kpd + UMS9117_KPD_CLK_DIVIDE);
	writel(16, keypad->kpd + UMS9117_KPD_DEBOUNCE);
	/* The matrix interrupt and scanning start once the IRQ is requested. */
	writel(0, keypad->kpd + UMS9117_KPD_INT_EN);
	writel(0xffff, keypad->kpd + UMS9117_KPD_POLARITY);

	ctrl = readl(keypad->kpd + UMS9117_KPD_CTRL);
	ctrl |= UMS9117_KPD_CTRL_LONG_KEY;
	ctrl &= ~UMS9117_KPD_CTRL_ENABLE;
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

static int ums9117_keypad_get_matrix_irq(struct platform_device *pdev,
					 struct ums9117_keypad *keypad)
{
	struct irq_data *irq_data;
	irq_hw_number_t hwirq;
	u32 trigger;
	int irq;

	irq = platform_get_irq_byname(pdev, "matrix");
	if (irq < 0)
		return dev_err_probe(keypad->dev, irq,
				     "could not resolve matrix IRQ\n");

	irq_data = irq_get_irq_data(irq);
	if (!irq_data)
		return dev_err_probe(keypad->dev, -EINVAL,
				     "matrix IRQ has no IRQ domain data\n");
	hwirq = irqd_to_hwirq(irq_data);
	trigger = irqd_get_trigger_type(irq_data);
	if (hwirq != UMS9117_KPD_MATRIX_IRQ_HWIRQ ||
	    trigger != IRQ_TYPE_LEVEL_HIGH)
		return dev_err_probe(
			keypad->dev, -EINVAL,
			"matrix IRQ must be SPI%u/hwirq%u level-high, got hwirq%lu type 0x%x\n",
			UMS9117_KPD_MATRIX_IRQ_SPI,
			UMS9117_KPD_MATRIX_IRQ_HWIRQ, (unsigned long)hwirq,
			trigger);

	keypad->matrix_irq = irq;
	return 0;
}

static void ums9117_keypad_start_matrix_irq(struct ums9117_keypad *keypad)
{
	u32 ctrl;

	writel(UMS9117_KPD_INT_OWNED_MASK, keypad->kpd + UMS9117_KPD_INT_CLR);
	writel(UMS9117_KPD_INT_OWNED_MASK, keypad->kpd + UMS9117_KPD_INT_EN);
	ctrl = readl(keypad->kpd + UMS9117_KPD_CTRL);
	ctrl |= UMS9117_KPD_CTRL_ENABLE;
	writel(ctrl, keypad->kpd + UMS9117_KPD_CTRL);
}

static int ums9117_keypad_get_eic_key(struct ums9117_keypad *keypad,
				      struct ums9117_keypad_eic_key *key,
				      const char *con_id,
				      const char *keycode_property,
				      unsigned int scancode)
{
	u32 keycode;
	int ret;

	key->keypad = keypad;
	key->scancode = scancode;
	key->irq = -1;
	key->gpiod = devm_gpiod_get_optional(keypad->dev, con_id, GPIOD_IN);
	if (IS_ERR(key->gpiod))
		return dev_err_probe(keypad->dev, PTR_ERR(key->gpiod),
				     "could not acquire %s GPIO\n", con_id);

	if (!key->gpiod) {
		if (device_property_present(keypad->dev, keycode_property))
			return dev_err_probe(keypad->dev, -EINVAL,
					     "%s requires a matching GPIO\n",
					     keycode_property);
		return 0;
	}

	ret = device_property_read_u32(keypad->dev, keycode_property, &keycode);
	if (ret)
		return dev_err_probe(keypad->dev, ret,
				     "%s GPIO has no keycode\n", con_id);
	if (keycode > KEY_MAX)
		return dev_err_probe(keypad->dev, -EINVAL,
				     "%s keycode is outside the input ABI\n",
				     con_id);
	keypad->keymap[scancode] = keycode;

	key->irq = gpiod_to_irq(key->gpiod);
	if (key->irq < 0)
		return dev_err_probe(keypad->dev, key->irq,
				     "could not resolve %s GPIO IRQ\n", con_id);

	return 0;
}

static int ums9117_keypad_parse_eic(struct ums9117_keypad *keypad)
{
	int ret;

	ret = ums9117_keypad_get_eic_key(keypad, &keypad->eic1, "eic1",
					 "sprd,eic1-keycode",
					 UMS9117_KPD_EIC1_SCANCODE);
	if (ret)
		return ret;

	return ums9117_keypad_get_eic_key(keypad, &keypad->eic9, "eic9",
					  "sprd,eic9-keycode",
					  UMS9117_KPD_EIC9_SCANCODE);
}

static int ums9117_keypad_request_eic_irq(struct ums9117_keypad_eic_key *key)
{
	if (!key->gpiod)
		return 0;

	return devm_request_threaded_irq(
		key->keypad->dev, key->irq, NULL, ums9117_keypad_eic_irq_thread,
		IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING |
			IRQF_TRIGGER_FALLING,
		dev_name(key->keypad->dev), key);
}

static int ums9117_keypad_start_eic_irqs(struct ums9117_keypad *keypad)
{
	struct ums9117_keypad_eic_key *keys[] = {
		&keypad->eic1,
		&keypad->eic9,
	};
	bool sync = false;
	unsigned int index;
	int ret;

	for (index = 0; index < ARRAY_SIZE(keys); ++index) {
		ret = ums9117_keypad_sample_eic_key(keys[index], true);
		if (ret < 0)
			return ret;
		sync |= ret;
	}
	if (sync)
		input_sync(keypad->input);

	for (index = 0; index < ARRAY_SIZE(keys); ++index) {
		if (!keys[index]->gpiod)
			continue;
		enable_irq(keys[index]->irq);
		keys[index]->irq_enabled = true;
	}

	return 0;
}

static int ums9117_keypad_suspend(struct device *dev)
{
	struct ums9117_keypad *controller = dev_get_drvdata(dev);
	int ret;

	/*
	 * Nested EIC child IRQs are not masked by suspend_device_irqs(). EIC9
	 * therefore has to be disabled explicitly before the shared parent gets
	 * wake permission for EIC1.
	 */
	if (controller->eic9.irq_enabled &&
	    !controller->eic9.suspend_disabled) {
		disable_irq(controller->eic9.irq);
		controller->eic9.suspend_disabled = true;
	}

	if (!device_may_wakeup(dev) || controller->eic1.wake_enabled)
		return 0;

	ret = enable_irq_wake(controller->eic1.irq);
	if (ret) {
		if (controller->eic9.suspend_disabled) {
			enable_irq(controller->eic9.irq);
			controller->eic9.suspend_disabled = false;
		}
		dev_err(dev, "could not enable EIC1 power-key wake IRQ: %pe\n",
			ERR_PTR(ret));
		return ret;
	}

	controller->eic1.wake_enabled = true;
	return 0;
}

static int ums9117_keypad_resume(struct device *dev)
{
	struct ums9117_keypad *controller = dev_get_drvdata(dev);
	int ret;

	if (controller->eic1.wake_enabled) {
		ret = disable_irq_wake(controller->eic1.irq);
		if (ret) {
			dev_err(dev,
				"could not disable EIC1 power-key wake IRQ: %pe\n",
				ERR_PTR(ret));
			return ret;
		}

		controller->eic1.wake_enabled = false;
	}
	if (controller->eic9.suspend_disabled) {
		ret = ums9117_keypad_sample_eic_key(&controller->eic9, true);
		enable_irq(controller->eic9.irq);
		controller->eic9.suspend_disabled = false;
		if (ret < 0) {
			dev_err(dev,
				"could not resample EIC9 after system sleep: %pe\n",
				ERR_PTR(ret));
			return ret;
		}
		if (ret)
			input_sync(controller->input);
	}
	return 0;
}

static int ums9117_keypad_disarm_eic1_wake(struct ums9117_keypad *keypad)
{
	int ret;

	if (!keypad->eic1.wake_enabled)
		return 0;

	ret = disable_irq_wake(keypad->eic1.irq);
	if (!ret)
		keypad->eic1.wake_enabled = false;

	return ret;
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
	if (device_property_present(dev, "wakeup-source") &&
	    !keypad->eic1.gpiod)
		return dev_err_probe(
			dev, -EINVAL,
			"wakeup-source requires EIC1 power-key GPIO\n");
	ret = ums9117_keypad_get_matrix_irq(pdev, keypad);
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
					 keypad->keymap, input);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to build matrix keymap\n");
	/* The matrix builder limits the table to the matrix; include EIC keys. */
	input->keycodemax = ARRAY_SIZE(keypad->keymap);
	input_set_capability(input, EV_MSC, MSC_SCAN);
	if (keypad->eic1.gpiod)
		input_set_capability(input, EV_KEY,
				     keypad->keymap[UMS9117_KPD_EIC1_SCANCODE]);
	if (keypad->eic9.gpiod)
		input_set_capability(input, EV_KEY,
				     keypad->keymap[UMS9117_KPD_EIC9_SCANCODE]);
	if (device_property_present(dev, "wakeup-source")) {
		ret = devm_device_init_wakeup(dev);
		if (ret)
			return dev_err_probe(
				dev, ret,
				"could not enable keypad wake capability\n");
	}

	ret = ums9117_keypad_hw_init(keypad);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, keypad->matrix_irq,
			       ums9117_keypad_matrix_irq, 0, dev_name(dev),
			       keypad);
	if (ret) {
		dev_err_probe(dev, ret, "could not request matrix IRQ\n");
		goto err_mask_matrix;
	}
	ret = ums9117_keypad_request_eic_irq(&keypad->eic1);
	if (ret) {
		dev_err_probe(dev, ret, "could not request EIC1 IRQ\n");
		goto err_mask_matrix;
	}
	ret = ums9117_keypad_request_eic_irq(&keypad->eic9);
	if (ret) {
		dev_err_probe(dev, ret, "could not request EIC9 IRQ\n");
		goto err_mask_matrix;
	}
	ret = input_register_device(input);
	if (ret) {
		dev_err_probe(dev, ret, "could not register keypad input\n");
		goto err_mask_matrix;
	}

	ret = ums9117_keypad_start_eic_irqs(keypad);
	if (ret) {
		dev_err_probe(dev, ret, "could not sample EIC GPIO\n");
		goto err_mask_matrix;
	}
	platform_set_drvdata(pdev, keypad);
	ums9117_keypad_start_matrix_irq(keypad);

	dev_dbg(dev, "keypad registered with matrix IRQ SPI%u/hwirq%u%s\n",
		UMS9117_KPD_MATRIX_IRQ_SPI, UMS9117_KPD_MATRIX_IRQ_HWIRQ,
		keypad->eic1.gpiod || keypad->eic9.gpiod ?
			" and EIC GPIO IRQs" :
			"");
	return 0;

err_mask_matrix:
	if (keypad->eic1.down) {
		keypad->eic1.down = false;
		ums9117_keypad_notify_power(keypad, false);
	}
	ums9117_keypad_mask_matrix_irq(keypad);
	return ret;
}

static void ums9117_keypad_stop(struct ums9117_keypad *controller)
{
	int ret;

	WRITE_ONCE(controller->stopping, true);
	ums9117_keypad_mask_matrix_irq(controller);
	synchronize_irq(controller->matrix_irq);
	ret = ums9117_keypad_disarm_eic1_wake(controller);
	if (ret)
		dev_err(controller->dev,
			"could not disable EIC1 power-key wake IRQ: %pe\n",
			ERR_PTR(ret));
	if (controller->eic9.suspend_disabled) {
		enable_irq(controller->eic9.irq);
		controller->eic9.suspend_disabled = false;
	}
	if (controller->eic1.irq_enabled) {
		disable_irq(controller->eic1.irq);
		synchronize_irq(controller->eic1.irq);
		controller->eic1.irq_enabled = false;
	}
	if (controller->eic1.down) {
		controller->eic1.down = false;
		ums9117_keypad_notify_power(controller, false);
	}
	if (controller->eic9.irq_enabled) {
		disable_irq(controller->eic9.irq);
		synchronize_irq(controller->eic9.irq);
		controller->eic9.irq_enabled = false;
	}
}

static DEFINE_SIMPLE_DEV_PM_OPS(ums9117_keypad_pm_ops, ums9117_keypad_suspend,
				ums9117_keypad_resume);

static void ums9117_keypad_remove(struct platform_device *pdev)
{
	ums9117_keypad_stop(platform_get_drvdata(pdev));
}

static void ums9117_keypad_shutdown(struct platform_device *pdev)
{
	ums9117_keypad_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id ums9117_keypad_of_match[] = {
	{ .compatible = "sprd,ums9117-keypad" },
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
		.pm = pm_sleep_ptr(&ums9117_keypad_pm_ops),
	},
};
module_platform_driver(ums9117_keypad_driver);

MODULE_DESCRIPTION("UMS9117 keypad with matrix and optional EIC GPIO IRQs");
MODULE_LICENSE("GPL");
