// SPDX-License-Identifier: GPL-2.0-only
/*
 * Polled matrix-keypad driver for Nokia 3210 4G TA-1618.
 *
 * The register layout and scan-code decoding are taken from the working
 * UMS9117 fpdoom port.  The driver polls because the keypad interrupt route is
 * not validated.  The separate physical 8 key is sampled through the inherited
 * over ADI.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/sprd/ums9117-adi.h>
#include <linux/workqueue.h>

#define KPD_CTRL 0x00
#define KPD_INT_RAW 0x08
#define KPD_INT_CLR 0x10
#define KPD_POLARITY 0x18
#define KPD_DEBOUNCE 0x1c
#define KPD_CLK_DIVIDE 0x28
#define KPD_KEY_STATUS 0x2c

#define APB_PWR_SET 0x1000
#define APB_CLK_SET 0x1010
#define APB_RST_SET 0x1008
#define APB_RST_CLR 0x2008

#define UMS9117_ADI_PHYS 0x40600000u
#define UMS9117_ADI_SLAVE_PHYS 0x40608000u
#define UMS9117_ADI_SLAVE_SIZE 0x1000u
#define ADI_CONTROLLER_MIN_SIZE 0x228u

#define EIC_DATA_BIT 9
#define EIC_POLL_MS 5

struct ta1618_keypad {
	struct device *dev;
	void __iomem *kpd;
	u32 eic_data_phys;
	struct regmap *aon_apb;
	struct input_dev *input;
	struct delayed_work poll_work;
	struct delayed_work eic_poll_work;
	bool eic_available;
	bool eic_baseline_valid;
	bool eic_baseline;
	bool eic_down;
	bool eic_error_reported;
	bool eic_first_event_reported;
	bool stopping;
};

/*
 * Exact TA-1618 scan map derived from the proven 8x8 fpdoom keymap.bin.
 * Unlisted scan positions are physically absent.
 */
static const unsigned short ta1618_keycodes[64] = {
	[0] = KEY_KPASTERISK, /* * */
	[1] = KEY_0,	      [2] = KEY_TAB, /* left soft */
	[3] = KEY_KPDOT, /* # */
	[8] = KEY_7,	      [9] = KEY_3,	    [10] = KEY_LEFT,
	[11] = KEY_9,	      [16] = KEY_1,	    [17] = KEY_2,
	[19] = KEY_6,	      [24] = KEY_ENTER, /* dial */
	[25] = KEY_DOWN,      [26] = KEY_ENTER, /* centre */
	[27] = KEY_RIGHT,     [32] = KEY_4,	    [33] = KEY_5,
	[34] = KEY_UP,	      [35] = KEY_BACKSPACE, /* right soft */
};

static void ta1618_keypad_poll(struct work_struct *work)
{
	struct ta1618_keypad *tk = container_of(
		to_delayed_work(work), struct ta1618_keypad, poll_work);
	u32 event = readl(tk->kpd + KPD_INT_RAW) & 0xff;
	u32 status;
	unsigned int i;
	bool sync = false;

	if (!event)
		goto out;

	status = readl(tk->kpd + KPD_KEY_STATUS);
	writel(0xfff, tk->kpd + KPD_INT_CLR);
	if (status & BIT(3))
		goto out;

	for (i = 0; i < 8; ++i) {
		unsigned int scan;
		unsigned int packed;
		unsigned short code;
		bool down;

		if (!(event & BIT(i)))
			continue;

		packed = status >> ((i & 3) * 8);
		scan = ((packed & 0x70) >> 1) | (packed & 0x07);
		code = ta1618_keycodes[scan & 0x3f];
		down = i < 4;
		if (!code)
			continue;

		input_report_key(tk->input, code, down);
		sync = true;
	}

	if (sync)
		input_sync(tk->input);
out:
	if (!READ_ONCE(tk->stopping))
		schedule_delayed_work(&tk->poll_work, msecs_to_jiffies(5));
}

static int ta1618_keypad_adi_read(struct ta1618_keypad *tk, u16 *value)
{
	struct ums9117_adi_transaction transaction = {};
	u32 offset = tk->eic_data_phys - UMS9117_ADI_SLAVE_PHYS;
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

static void ta1618_keypad_eic_poll(struct work_struct *work)
{
	struct ta1618_keypad *tk = container_of(
		to_delayed_work(work), struct ta1618_keypad, eic_poll_work);
	bool down;
	bool level;
	u16 data;
	int ret;

	ret = ta1618_keypad_adi_read(tk, &data);
	if (ret) {
		if (!tk->eic_error_reported) {
			dev_warn(
				tk->dev,
				"EIC9 read unavailable (%d); matrix keypad remains active\n",
				ret);
			tk->eic_error_reported = true;
		}
		goto out;
	}

	level = !!(data & BIT(EIC_DATA_BIT));
	if (!tk->eic_baseline_valid) {
		/*
		 * Preserve the inherited polarity: the first readable idle
		 * level becomes the release baseline and emits no input event.
		 */
		tk->eic_baseline = level;
		tk->eic_baseline_valid = true;
		goto out;
	}

	down = level != tk->eic_baseline;
	if (down != tk->eic_down) {
		tk->eic_down = down;
		input_report_key(tk->input, KEY_8, down);
		input_sync(tk->input);
		if (!tk->eic_first_event_reported) {
			tk->eic_first_event_reported = true;
			dev_info(tk->dev,
				 "first EIC9 key event: down=%u data=0x%04x\n",
				 down, data);
		}
	}

out:
	if (!READ_ONCE(tk->stopping))
		schedule_delayed_work(&tk->eic_poll_work,
				      msecs_to_jiffies(EIC_POLL_MS));
}

static void ta1618_keypad_setup_eic(struct platform_device *pdev,
				    struct ta1618_keypad *tk)
{
	struct device *dev = &pdev->dev;
	struct resource *adi_res;
	int ret;

	adi_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "adi");
	ret = of_property_read_u32(dev->of_node, "sprd,eic-data-address",
				   &tk->eic_data_phys);
	if (!adi_res || ret) {
		dev_warn(
			dev,
			"EIC9 resources unavailable; matrix keypad remains active\n");
		return;
	}
	if (adi_res->start != UMS9117_ADI_PHYS ||
	    resource_size(adi_res) < ADI_CONTROLLER_MIN_SIZE ||
	    !IS_ALIGNED(tk->eic_data_phys, sizeof(u32)) ||
	    tk->eic_data_phys < UMS9117_ADI_SLAVE_PHYS ||
	    tk->eic_data_phys > UMS9117_ADI_SLAVE_PHYS +
					UMS9117_ADI_SLAVE_SIZE - sizeof(u32)) {
		dev_warn(
			dev,
			"EIC9 resources invalid; matrix keypad remains active\n");
		return;
	}

	tk->eic_available = true;
}

static int ta1618_keypad_hw_init(struct ta1618_keypad *tk)
{
	u32 ctrl;
	int ret;

	/* Exact clock/reset sequence used by fpdoom on UMS9117. */
	ret = regmap_write(tk->aon_apb, APB_PWR_SET, 0x100);
	if (ret)
		return ret;
	ret = regmap_write(tk->aon_apb, APB_CLK_SET, 0x002);
	if (ret)
		return ret;
	ret = regmap_write(tk->aon_apb, APB_RST_SET, 0x100);
	if (ret)
		return ret;
	udelay(100);
	ret = regmap_write(tk->aon_apb, APB_RST_CLR, 0x100);
	if (ret)
		return ret;
	udelay(100);

	writel(0xfff, tk->kpd + KPD_INT_CLR);
	writel(1, tk->kpd + KPD_CLK_DIVIDE);
	writel(16, tk->kpd + KPD_DEBOUNCE);
	writel(0xffff, tk->kpd + KPD_POLARITY);

	ctrl = readl(tk->kpd + KPD_CTRL);
	ctrl |= BIT(0); /* enable */
	ctrl &= ~BIT(1); /* no sleep */
	ctrl |= BIT(2); /* long-key detection */
	ctrl &= ~0x00fcfc00;
	ctrl |= 0x001c0c00; /* active rows 2..4, columns 2..3 */
	writel(ctrl, tk->kpd + KPD_CTRL);
	return 0;
}

static int ta1618_keypad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ta1618_keypad *tk;
	struct input_dev *input;
	unsigned int i;
	int ret;

	tk = devm_kzalloc(dev, sizeof(*tk), GFP_KERNEL);
	if (!tk)
		return -ENOMEM;
	tk->dev = dev;

	tk->kpd = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(tk->kpd))
		return PTR_ERR(tk->kpd);
	tk->aon_apb =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,aon-apb");
	if (IS_ERR(tk->aon_apb))
		return dev_err_probe(dev, PTR_ERR(tk->aon_apb),
				     "could not resolve AON APB syscon\n");
	ta1618_keypad_setup_eic(pdev, tk);

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	tk->input = input;
	input->name = "TA-1618 keypad";
	input->phys = "ta1618/keypad0";
	input->id.bustype = BUS_HOST;

	for (i = 0; i < ARRAY_SIZE(ta1618_keycodes); ++i)
		if (ta1618_keycodes[i])
			input_set_capability(input, EV_KEY, ta1618_keycodes[i]);
	if (tk->eic_available)
		input_set_capability(input, EV_KEY, KEY_8);

	ret = ta1618_keypad_hw_init(tk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "could not initialize keypad clocks\n");
	ret = input_register_device(input);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&tk->poll_work, ta1618_keypad_poll);
	INIT_DELAYED_WORK(&tk->eic_poll_work, ta1618_keypad_eic_poll);
	platform_set_drvdata(pdev, tk);
	schedule_delayed_work(&tk->poll_work, msecs_to_jiffies(5));
	if (tk->eic_available)
		schedule_delayed_work(&tk->eic_poll_work,
				      msecs_to_jiffies(EIC_POLL_MS));

	dev_info(dev, "polled TA-1618 keypad registered (EIC9 %s)\n",
		 tk->eic_available ? "5 ms poll" : "unavailable");
	return 0;
}

static void ta1618_keypad_stop(struct ta1618_keypad *tk)
{
	WRITE_ONCE(tk->stopping, true);
	cancel_delayed_work_sync(&tk->poll_work);
	if (tk->eic_available)
		cancel_delayed_work_sync(&tk->eic_poll_work);
}

static void ta1618_keypad_remove(struct platform_device *pdev)
{
	ta1618_keypad_stop(platform_get_drvdata(pdev));
}

static void ta1618_keypad_shutdown(struct platform_device *pdev)
{
	ta1618_keypad_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id ta1618_keypad_of_match[] = {
	{ .compatible = "fplinux,ta1618-keypad" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_keypad_of_match);

static struct platform_driver ta1618_keypad_driver = {
	.probe = ta1618_keypad_probe,
	.remove = ta1618_keypad_remove,
	.shutdown = ta1618_keypad_shutdown,
	.driver = {
		.name = "ta1618-keypad",
		.of_match_table = ta1618_keypad_of_match,
	},
};
module_platform_driver(ta1618_keypad_driver);

MODULE_DESCRIPTION("Nokia TA-1618 polled matrix and EIC9 keypad");
MODULE_LICENSE("GPL");
