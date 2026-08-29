// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_GPIO_SC2720_EIC_UMS9117)

/*
 * SC2720 analog EIC GPIO and IRQ provider for UMS9117.
 *
 * The analog EIC bank is reachable only through the platform-wide ADI
 * transport. SC2720 INTA owns the shared analog aggregate on GIC SPI38; this
 * driver consumes only its EICA child source. It deliberately accepts only
 * the qualified UMS9117/SC2720 state left by the RAM bootstrap instead of
 * trying to initialize the PMIC.
 */
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/sprd/ums9117-adi.h>

#define SC2720_INTA_SOURCE_EICA 4U

#define SC2720_EICA_DATA 0x280U
#define SC2720_EICA_DMSK 0x284U
#define SC2720_EICA_IEV 0x294U
#define SC2720_EICA_IE 0x298U
#define SC2720_EICA_RIS 0x29cU
#define SC2720_EICA_MIS 0x2a0U
#define SC2720_EICA_IC 0x2a4U
#define SC2720_EICA_TRIG 0x2a8U
#define SC2720_EICA_CTRL0 0x2c0U
#define SC2720_EICA_CTRL(line) (SC2720_EICA_CTRL0 + (line) * sizeof(u32))
#define SC2720_EICA_CTRL_EXPECTED 0x4032U
#define SC2720_EICA_LINES 13U
#define SC2720_EICA_OWNED_MASK GENMASK(SC2720_EICA_LINES - 1, 0)

struct ums9117_sc2720_eic {
	struct device *dev;
	struct gpio_chip chip;
	struct mutex lock;
	u16 baseline_dmsk;
	u16 enabled;
	u16 irq_dirty;
	u16 wake_mask;
	int parent_irq;
	bool parent_enabled;
	bool published;
	bool failed;
};

static int ums9117_sc2720_eic_adi_read(u32 offset, u16 *value)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, offset, value);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int ums9117_sc2720_eic_adi_write(u32 offset, u16 value)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_write(&transaction, offset, value);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int ums9117_sc2720_eic_adi_update(u32 offset, u16 mask, u16 value)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits(&transaction, offset, mask, value);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int ums9117_sc2720_eic_adi_command(u32 offset, u16 value)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_write_final(&transaction, offset, value);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int ums9117_sc2720_eic_read_baseline(struct ums9117_sc2720_eic *eic)
{
	u16 iev;
	u16 ie;
	u16 ris;
	u16 mis;
	int ret;

	if (ums9117_adi_is_poisoned())
		return -EIO;

	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_DMSK,
					  &eic->baseline_dmsk);
	if (ret)
		return ret;
	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_IEV, &iev);
	if (ret)
		return ret;
	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_IE, &ie);
	if (ret)
		return ret;
	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_RIS, &ris);
	if (ret)
		return ret;
	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_MIS, &mis);
	if (ret)
		return ret;
	if ((eic->baseline_dmsk & ~SC2720_EICA_OWNED_MASK) ||
	    iev != GENMASK(15, 0) || ie || ris || mis)
		return -EPROTO;

	return 0;
}

static void ums9117_sc2720_eic_disable_parent(struct ums9117_sc2720_eic *eic)
{
	if (!eic->parent_enabled)
		return;

	disable_irq_nosync(eic->parent_irq);
	eic->parent_enabled = false;
}

static void ums9117_sc2720_eic_fail_locked(struct ums9117_sc2720_eic *eic,
					   const char *where, int error)
{
	int ie_ret;

	if (eic->failed)
		return;

	eic->failed = true;
	ums9117_sc2720_eic_disable_parent(eic);
	ie_ret = ums9117_sc2720_eic_adi_write(SC2720_EICA_IE, 0);
	dev_err(eic->dev, "SC2720 EIC disabled at %s: %d (EICA IE %d)\n", where,
		error, ie_ret);
}

static int ums9117_sc2720_eic_set_polarity(struct ums9117_sc2720_eic *eic,
					   unsigned int offset)
{
	u16 data;
	u16 bit = BIT(offset);
	u16 value;
	int ret;

	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_DATA, &data);
	if (ret)
		return ret;
	value = (data & bit) ? 0 : bit;

	return ums9117_sc2720_eic_adi_update(SC2720_EICA_IEV, bit, value);
}

static int ums9117_sc2720_eic_sync_line(struct ums9117_sc2720_eic *eic,
					unsigned int offset)
{
	u16 bit = BIT(offset);
	int ret;

	if (!(eic->enabled & bit))
		return ums9117_sc2720_eic_adi_update(SC2720_EICA_IE, bit, 0);

	ret = ums9117_sc2720_eic_set_polarity(eic, offset);
	if (ret)
		return ret;
	ret = ums9117_sc2720_eic_adi_command(SC2720_EICA_IC, bit);
	if (ret)
		return ret;
	ret = ums9117_sc2720_eic_adi_update(SC2720_EICA_IE, bit, bit);
	if (ret)
		return ret;
	return ums9117_sc2720_eic_adi_command(SC2720_EICA_TRIG, bit);
}

static int ums9117_sc2720_eic_rearm_edge_both(struct ums9117_sc2720_eic *eic,
					      unsigned int offset)
{
	u16 bit = BIT(offset);
	int ret;

	mutex_lock(&eic->lock);
	if (eic->failed || !(eic->enabled & bit))
		goto out;

	ret = ums9117_sc2720_eic_set_polarity(eic, offset);
	if (ret)
		goto fail;
	ret = ums9117_sc2720_eic_adi_update(SC2720_EICA_IE, bit, bit);
	if (ret)
		goto fail;
	ret = ums9117_sc2720_eic_adi_command(SC2720_EICA_TRIG, bit);
	if (ret)
		goto fail;
	goto out;

fail:
	ums9117_sc2720_eic_fail_locked(eic, "edge rearm", ret);
out:
	mutex_unlock(&eic->lock);
	return eic->failed ? -EIO : 0;
}

static irqreturn_t ums9117_sc2720_eic_parent_thread(int irq, void *data)
{
	struct ums9117_sc2720_eic *eic = data;
	unsigned long pending;
	u16 post_mis;
	u16 post_ris;
	u16 mis;
	unsigned int offset;
	int ret;

	(void)irq;
	mutex_lock(&eic->lock);
	if (eic->failed)
		goto out_unlock;

	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_MIS, &mis);
	if (ret)
		goto fail;

	if ((mis & ~SC2720_EICA_OWNED_MASK)) {
		ret = -EPROTO;
		goto fail;
	}

	pending = mis & SC2720_EICA_OWNED_MASK;
	if (!pending || (pending & ~eic->enabled)) {
		ret = -EPROTO;
		goto fail;
	}

	ret = ums9117_sc2720_eic_adi_update(SC2720_EICA_IE, pending, 0);
	if (ret)
		goto fail;
	ret = ums9117_sc2720_eic_adi_command(SC2720_EICA_IC, pending);
	if (ret)
		goto fail;
	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_RIS, &post_ris);
	if (ret)
		goto fail;
	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_MIS, &post_mis);
	if (ret)
		goto fail;
	if ((post_ris & pending) || (post_mis & pending) ||
	    (post_mis & ~SC2720_EICA_OWNED_MASK) ||
	    (post_mis & ~eic->enabled)) {
		ret = -EPROTO;
		goto fail;
	}
	mutex_unlock(&eic->lock);

	for_each_set_bit(offset, &pending, SC2720_EICA_LINES) {
		unsigned int child_irq;

		child_irq = irq_find_mapping(eic->chip.irq.domain, offset);
		if (!child_irq) {
			mutex_lock(&eic->lock);
			ums9117_sc2720_eic_fail_locked(eic, "child mapping",
						       -ENXIO);
			mutex_unlock(&eic->lock);
			return IRQ_HANDLED;
		}
		handle_nested_irq(child_irq);
		if (ums9117_sc2720_eic_rearm_edge_both(eic, offset))
			return IRQ_HANDLED;
	}

	return IRQ_HANDLED;

fail:
	ums9117_sc2720_eic_fail_locked(eic, "parent IRQ", ret);
out_unlock:
	mutex_unlock(&eic->lock);
	return IRQ_HANDLED;
}

static int ums9117_sc2720_eic_request(struct gpio_chip *chip,
				      unsigned int offset)
{
	struct ums9117_sc2720_eic *eic = gpiochip_get_data(chip);
	u16 bit = BIT(offset);
	u16 ctrl;
	int ret;

	mutex_lock(&eic->lock);
	if (eic->failed) {
		ret = -EIO;
		goto out;
	}
	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_CTRL(offset), &ctrl);
	if (ret)
		goto fail;
	if (ctrl != SC2720_EICA_CTRL_EXPECTED) {
		ret = -EPROTO;
		goto out;
	}
	ret = ums9117_sc2720_eic_adi_update(SC2720_EICA_DMSK, bit, bit);
	if (ret)
		goto fail;
	goto out;
fail:
	ums9117_sc2720_eic_fail_locked(eic, "GPIO request", ret);
out:
	mutex_unlock(&eic->lock);
	return ret;
}

static void ums9117_sc2720_eic_free(struct gpio_chip *chip, unsigned int offset)
{
	struct ums9117_sc2720_eic *eic = gpiochip_get_data(chip);
	u16 bit = BIT(offset);
	int ret;

	mutex_lock(&eic->lock);
	if (!eic->failed) {
		ret = ums9117_sc2720_eic_adi_update(SC2720_EICA_DMSK, bit,
						    eic->baseline_dmsk & bit);
		if (ret)
			ums9117_sc2720_eic_fail_locked(eic, "GPIO release",
						       ret);
	}
	mutex_unlock(&eic->lock);
}

static int ums9117_sc2720_eic_get(struct gpio_chip *chip, unsigned int offset)
{
	struct ums9117_sc2720_eic *eic = gpiochip_get_data(chip);
	u16 data;
	int ret;

	mutex_lock(&eic->lock);
	if (eic->failed) {
		ret = -EIO;
		goto out;
	}
	ret = ums9117_sc2720_eic_adi_read(SC2720_EICA_DATA, &data);
	if (!ret)
		ret = !!(data & BIT(offset));
out:
	mutex_unlock(&eic->lock);
	return ret;
}

static int ums9117_sc2720_eic_direction_input(struct gpio_chip *chip,
					      unsigned int offset)
{
	return 0;
}

static void ums9117_sc2720_eic_irq_mask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct ums9117_sc2720_eic *eic = gpiochip_get_data(chip);
	unsigned int offset = irqd_to_hwirq(data);

	eic->enabled &= ~BIT(offset);
	eic->irq_dirty |= BIT(offset);
	gpiochip_disable_irq(chip, offset);
}

static void ums9117_sc2720_eic_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct ums9117_sc2720_eic *eic = gpiochip_get_data(chip);
	unsigned int offset = irqd_to_hwirq(data);

	gpiochip_enable_irq(chip, offset);
	eic->enabled |= BIT(offset);
	eic->irq_dirty |= BIT(offset);
}

static int ums9117_sc2720_eic_irq_set_type(struct irq_data *data,
					   unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct ums9117_sc2720_eic *eic = gpiochip_get_data(chip);
	unsigned int offset = irqd_to_hwirq(data);

	if (type != IRQ_TYPE_EDGE_BOTH)
		return -EINVAL;

	eic->irq_dirty |= BIT(offset);
	irq_set_handler_locked(data, handle_edge_irq);
	return 0;
}

static int ums9117_sc2720_eic_irq_set_wake(struct irq_data *data,
					   unsigned int on)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct ums9117_sc2720_eic *eic = gpiochip_get_data(chip);
	u16 bit = BIT(irqd_to_hwirq(data));
	int ret;

	/* The IRQ core already holds this chip's irq_bus_lock(). */
	if (eic->failed)
		return -EIO;

	if (on) {
		if (eic->wake_mask & bit)
			return 0;

		if (!eic->wake_mask) {
			ret = enable_irq_wake(eic->parent_irq);
			if (ret)
				return ret;
		}
		eic->wake_mask |= bit;
		return 0;
	}

	if (WARN_ON_ONCE(!(eic->wake_mask & bit)))
		return -EINVAL;

	eic->wake_mask &= ~bit;
	if (eic->wake_mask)
		return 0;

	ret = disable_irq_wake(eic->parent_irq);
	if (ret)
		eic->wake_mask |= bit;

	return ret;
}

static void ums9117_sc2720_eic_irq_bus_lock(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct ums9117_sc2720_eic *eic = gpiochip_get_data(chip);

	mutex_lock(&eic->lock);
}

static void ums9117_sc2720_eic_irq_bus_sync_unlock(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct ums9117_sc2720_eic *eic = gpiochip_get_data(chip);
	unsigned int offset = irqd_to_hwirq(data);
	u16 bit = BIT(offset);
	int ret;

	if (!eic->failed && (eic->irq_dirty & bit)) {
		eic->irq_dirty &= ~bit;
		ret = ums9117_sc2720_eic_sync_line(eic, offset);
		if (ret)
			ums9117_sc2720_eic_fail_locked(eic, "child IRQ setup",
						       ret);
	}
	mutex_unlock(&eic->lock);
}

static const struct irq_chip ums9117_sc2720_eic_irq_chip = {
	.name = "ums9117-sc2720-eic",
	.irq_disable = ums9117_sc2720_eic_irq_mask,
	.irq_enable = ums9117_sc2720_eic_irq_unmask,
	.irq_mask = ums9117_sc2720_eic_irq_mask,
	.irq_unmask = ums9117_sc2720_eic_irq_unmask,
	.irq_set_type = ums9117_sc2720_eic_irq_set_type,
	.irq_set_wake = ums9117_sc2720_eic_irq_set_wake,
	.irq_bus_lock = ums9117_sc2720_eic_irq_bus_lock,
	.irq_bus_sync_unlock = ums9117_sc2720_eic_irq_bus_sync_unlock,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int ums9117_sc2720_eic_validate_parent(struct ums9117_sc2720_eic *eic)
{
	struct irq_data *irq_data;

	irq_data = irq_get_irq_data(eic->parent_irq);
	if (!irq_data || irqd_to_hwirq(irq_data) != SC2720_INTA_SOURCE_EICA ||
	    irqd_get_trigger_type(irq_data) != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;

	return 0;
}

static int ums9117_sc2720_eic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ums9117_sc2720_eic *eic;
	struct gpio_irq_chip *irq;
	bool chip_added = false;
	bool parent_requested = false;
	int ret;

	eic = devm_kzalloc(dev, sizeof(*eic), GFP_KERNEL);
	if (!eic)
		return -ENOMEM;
	eic->dev = dev;
	mutex_init(&eic->lock);

	eic->parent_irq = platform_get_irq(pdev, 0);
	if (eic->parent_irq < 0)
		return dev_err_probe(
			dev, eic->parent_irq,
			"could not resolve SC2720 EIC parent IRQ\n");
	ret = ums9117_sc2720_eic_validate_parent(eic);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"parent IRQ must be SC2720 INTA source%u level-high\n",
			SC2720_INTA_SOURCE_EICA);

	ret = ums9117_sc2720_eic_read_baseline(eic);
	if (ret)
		return dev_err_probe(
			dev, ret, "SC2720 EIC inherited state is not usable\n");

	ret = request_threaded_irq(eic->parent_irq, NULL,
				   ums9117_sc2720_eic_parent_thread,
				   IRQF_ONESHOT | IRQF_NO_AUTOEN, dev_name(dev),
				   eic);
	if (ret)
		return dev_err_probe(
			dev, ret, "could not request SC2720 EIC parent IRQ\n");
	parent_requested = true;

	eic->chip.label = dev_name(dev);
	eic->chip.ngpio = SC2720_EICA_LINES;
	eic->chip.base = -1;
	eic->chip.parent = dev;
	eic->chip.request = ums9117_sc2720_eic_request;
	eic->chip.free = ums9117_sc2720_eic_free;
	eic->chip.get = ums9117_sc2720_eic_get;
	eic->chip.direction_input = ums9117_sc2720_eic_direction_input;
	eic->chip.can_sleep = true;

	irq = &eic->chip.irq;
	gpio_irq_chip_set_chip(irq, &ums9117_sc2720_eic_irq_chip);
	irq->threaded = true;

	ret = gpiochip_add_data(&eic->chip, eic);
	if (ret)
		goto out_restore;
	chip_added = true;

	enable_irq(eic->parent_irq);
	eic->parent_enabled = true;
	eic->published = true;
	platform_set_drvdata(pdev, eic);
	dev_info(dev, "SC2720 EIC registered on INTA source%u\n",
		 SC2720_INTA_SOURCE_EICA);
	return 0;

out_restore:
	if (eic->parent_enabled) {
		ums9117_sc2720_eic_disable_parent(eic);
		synchronize_irq(eic->parent_irq);
	}
	if (chip_added)
		gpiochip_remove(&eic->chip);
	if (parent_requested)
		free_irq(eic->parent_irq, eic);
	return dev_err_probe(dev, ret, "could not register SC2720 EIC\n");
}

static void ums9117_sc2720_eic_shutdown(struct platform_device *pdev)
{
	struct ums9117_sc2720_eic *eic = platform_get_drvdata(pdev);
	int ret;

	if (!eic || !eic->published)
		return;

	ums9117_sc2720_eic_disable_parent(eic);
	synchronize_irq(eic->parent_irq);
	mutex_lock(&eic->lock);
	eic->failed = true;
	ret = ums9117_sc2720_eic_adi_write(SC2720_EICA_IE, 0);
	if (ret)
		dev_err(eic->dev, "could not disable SC2720 EIC lines: %d\n",
			ret);
	ret = ums9117_sc2720_eic_adi_command(SC2720_EICA_IC,
					     SC2720_EICA_OWNED_MASK);
	if (ret)
		dev_err(eic->dev,
			"could not clear owned SC2720 EIC status: %d\n", ret);
	mutex_unlock(&eic->lock);
}

static const struct of_device_id ums9117_sc2720_eic_of_match[] = {
	{ .compatible = "sprd,ums9117-sc2720-eic" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_sc2720_eic_of_match);

static struct platform_driver ums9117_sc2720_eic_driver = {
	.probe = ums9117_sc2720_eic_probe,
	.shutdown = ums9117_sc2720_eic_shutdown,
	.driver = {
		.name = "ums9117-sc2720-eic",
		.of_match_table = ums9117_sc2720_eic_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ums9117_sc2720_eic_driver);

MODULE_DESCRIPTION("UMS9117 SC2720 analog EIC GPIO and IRQ controller");
MODULE_LICENSE("GPL");

#endif
