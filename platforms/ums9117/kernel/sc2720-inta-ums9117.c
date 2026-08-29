// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_IRQCHIP_SC2720_INTA_UMS9117)

/*
 * SC2720 analog interrupt aggregate for UMS9117.
 *
 * The SC2720 delivers several analog interrupt sources through one GIC line.
 * UMS9117 ADI is the sole transport to the PMIC, so this driver owns that
 * aggregate and exposes only the two sources with current Linux consumers:
 * RTC alarm (1) and analog EIC (4).
 */
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/sprd/ums9117-adi.h>

#define UMS9117_GIC_SPI_HWIRQ_BASE 32U
#define UMS9117_SC2720_INTA_SPI 38U
#define UMS9117_SC2720_INTA_HWIRQ \
	(UMS9117_GIC_SPI_HWIRQ_BASE + UMS9117_SC2720_INTA_SPI)

#define SC2720_ANA_INT_STATUS 0x0c0U
#define SC2720_ANA_INT_RAW 0x0c4U
#define SC2720_ANA_INT_ENABLE 0x0c8U
#define SC2720_ANA_INT_STATUS_SYNC 0x0ccU
#define SC2720_ANA_INT_ENABLE_BOOT GENMASK(8, 0)

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_MODULE_EN0 0xc08U
#define SC2720_RTC_CLK_EN0 0xc10U
#define SC2720_SOFT_RST0 0xc14U
#define SC2720_ARCH_EN 0xe40U
#define SC2720_CHIP_ID_LOW_EXPECTED 0xa003U
#define SC2720_CHIP_ID_HIGH_EXPECTED 0x2720U
#define SC2720_EIC_GATE BIT(3)
#define SC2720_ARCH_ENABLE BIT(0)

#define SC2720_INTA_SOURCE_RTC 1U
#define SC2720_INTA_SOURCE_EICA 4U
#define SC2720_INTA_DOMAIN_SIZE (SC2720_INTA_SOURCE_EICA + 1U)
#define SC2720_INTA_SUPPORTED_MASK \
	(BIT(SC2720_INTA_SOURCE_RTC) | BIT(SC2720_INTA_SOURCE_EICA))

struct ums9117_sc2720_inta {
	struct device *dev;
	struct irq_domain *domain;
	struct mutex lock;
	u16 enabled;
	u16 dirty;
	u16 wake_mask;
	int parent_irq;
	bool parent_enabled;
	bool failed;
	bool published;
};

static int ums9117_sc2720_inta_adi_read(u32 offset, u16 *value)
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

static int ums9117_sc2720_inta_adi_write(u32 offset, u16 value)
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

static bool ums9117_sc2720_inta_source_supported(irq_hw_number_t source)
{
	return source == SC2720_INTA_SOURCE_RTC ||
	       source == SC2720_INTA_SOURCE_EICA;
}

static int ums9117_sc2720_inta_read_baseline(struct ums9117_sc2720_inta *inta)
{
	u16 chip_id_low;
	u16 chip_id_high;
	u16 module_en0;
	u16 rtc_clk_en0;
	u16 soft_rst0;
	u16 arch_en;
	u16 status;
	u16 raw;
	u16 enable;
	u16 sync;
	int ret;

	if (ums9117_adi_is_poisoned())
		return -EIO;

	ret = ums9117_sc2720_inta_adi_read(SC2720_CHIP_ID_LOW, &chip_id_low);
	if (ret)
		return ret;
	ret = ums9117_sc2720_inta_adi_read(SC2720_CHIP_ID_HIGH, &chip_id_high);
	if (ret)
		return ret;
	ret = ums9117_sc2720_inta_adi_read(SC2720_MODULE_EN0, &module_en0);
	if (ret)
		return ret;
	ret = ums9117_sc2720_inta_adi_read(SC2720_RTC_CLK_EN0, &rtc_clk_en0);
	if (ret)
		return ret;
	ret = ums9117_sc2720_inta_adi_read(SC2720_SOFT_RST0, &soft_rst0);
	if (ret)
		return ret;
	ret = ums9117_sc2720_inta_adi_read(SC2720_ARCH_EN, &arch_en);
	if (ret)
		return ret;
	ret = ums9117_sc2720_inta_adi_read(SC2720_ANA_INT_STATUS, &status);
	if (ret)
		return ret;
	ret = ums9117_sc2720_inta_adi_read(SC2720_ANA_INT_RAW, &raw);
	if (ret)
		return ret;
	ret = ums9117_sc2720_inta_adi_read(SC2720_ANA_INT_ENABLE, &enable);
	if (ret)
		return ret;
	ret = ums9117_sc2720_inta_adi_read(SC2720_ANA_INT_STATUS_SYNC, &sync);
	if (ret)
		return ret;

	if (chip_id_low != SC2720_CHIP_ID_LOW_EXPECTED ||
	    chip_id_high != SC2720_CHIP_ID_HIGH_EXPECTED)
		return -ENODEV;
	if (!(module_en0 & SC2720_EIC_GATE) ||
	    !(rtc_clk_en0 & SC2720_EIC_GATE) || (soft_rst0 & SC2720_EIC_GATE) ||
	    !(arch_en & SC2720_ARCH_ENABLE))
		return -EHOSTDOWN;
	if (enable != SC2720_ANA_INT_ENABLE_BOOT || status || raw || sync)
		return -EPROTO;

	return 0;
}

static void ums9117_sc2720_inta_disable_parent(struct ums9117_sc2720_inta *inta)
{
	if (!inta->parent_enabled)
		return;

	disable_irq_nosync(inta->parent_irq);
	inta->parent_enabled = false;
}

static void ums9117_sc2720_inta_fail_locked(struct ums9117_sc2720_inta *inta,
					    const char *where, int error)
{
	int mask_ret;

	if (inta->failed)
		return;

	inta->failed = true;
	inta->enabled = 0;
	ums9117_sc2720_inta_disable_parent(inta);
	mask_ret = ums9117_sc2720_inta_adi_write(SC2720_ANA_INT_ENABLE, 0);
	dev_err(inta->dev, "SC2720 INTA disabled at %s: %d (ANA INT EN %d)\n",
		where, error, mask_ret);
}

static void ums9117_sc2720_inta_irq_mask(struct irq_data *data)
{
	struct ums9117_sc2720_inta *inta = irq_data_get_irq_chip_data(data);
	u16 bit = BIT(irqd_to_hwirq(data));

	inta->enabled &= ~bit;
	inta->dirty |= bit;
}

static void ums9117_sc2720_inta_irq_unmask(struct irq_data *data)
{
	struct ums9117_sc2720_inta *inta = irq_data_get_irq_chip_data(data);
	u16 bit = BIT(irqd_to_hwirq(data));

	inta->enabled |= bit;
	inta->dirty |= bit;
}

static int ums9117_sc2720_inta_irq_set_type(struct irq_data *data,
					    unsigned int type)
{
	if (type != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;

	irq_set_handler_locked(data, handle_level_irq);
	return 0;
}

static int ums9117_sc2720_inta_irq_set_wake(struct irq_data *data,
					    unsigned int on)
{
	struct ums9117_sc2720_inta *inta = irq_data_get_irq_chip_data(data);
	u16 bit = BIT(irqd_to_hwirq(data));
	int ret;

	/* The IRQ core already holds this chip's irq_bus_lock(). */
	if (inta->failed)
		return -EIO;

	if (on) {
		if (inta->wake_mask & bit)
			return 0;

		if (!inta->wake_mask) {
			ret = enable_irq_wake(inta->parent_irq);
			if (ret)
				return ret;
		}
		inta->wake_mask |= bit;
		return 0;
	}

	if (WARN_ON_ONCE(!(inta->wake_mask & bit)))
		return -EINVAL;

	inta->wake_mask &= ~bit;
	if (inta->wake_mask)
		return 0;

	ret = disable_irq_wake(inta->parent_irq);
	if (ret)
		inta->wake_mask |= bit;

	return ret;
}

static void ums9117_sc2720_inta_irq_bus_lock(struct irq_data *data)
{
	struct ums9117_sc2720_inta *inta = irq_data_get_irq_chip_data(data);

	mutex_lock(&inta->lock);
}

static void ums9117_sc2720_inta_irq_bus_sync_unlock(struct irq_data *data)
{
	struct ums9117_sc2720_inta *inta = irq_data_get_irq_chip_data(data);
	int ret;

	if (!inta->failed && inta->dirty) {
		inta->dirty = 0;
		ret = ums9117_sc2720_inta_adi_write(SC2720_ANA_INT_ENABLE,
						    inta->enabled);
		if (ret)
			ums9117_sc2720_inta_fail_locked(inta, "child IRQ setup",
							ret);
	}
	mutex_unlock(&inta->lock);
}

static const struct irq_chip ums9117_sc2720_inta_irq_chip = {
	.name = "ums9117-sc2720-inta",
	.irq_disable = ums9117_sc2720_inta_irq_mask,
	.irq_enable = ums9117_sc2720_inta_irq_unmask,
	.irq_mask = ums9117_sc2720_inta_irq_mask,
	.irq_unmask = ums9117_sc2720_inta_irq_unmask,
	.irq_set_type = ums9117_sc2720_inta_irq_set_type,
	.irq_set_wake = ums9117_sc2720_inta_irq_set_wake,
	.irq_bus_lock = ums9117_sc2720_inta_irq_bus_lock,
	.irq_bus_sync_unlock = ums9117_sc2720_inta_irq_bus_sync_unlock,
	.flags = IRQCHIP_IMMUTABLE,
};

static int ums9117_sc2720_inta_domain_map(struct irq_domain *domain,
					  unsigned int virq,
					  irq_hw_number_t hwirq)
{
	struct ums9117_sc2720_inta *inta = domain->host_data;
	int ret;

	if (!ums9117_sc2720_inta_source_supported(hwirq))
		return -EINVAL;

	irq_set_chip_data(virq, inta);
	irq_set_chip_and_handler(virq, &ums9117_sc2720_inta_irq_chip,
				 handle_level_irq);
	irq_set_nested_thread(virq, 1);
	ret = irq_set_parent(virq, inta->parent_irq);
	if (ret) {
		irq_set_nested_thread(virq, 0);
		irq_set_chip_and_handler(virq, NULL, NULL);
		irq_set_chip_data(virq, NULL);
		return ret;
	}
	irq_set_noprobe(virq);
	return 0;
}

static void ums9117_sc2720_inta_domain_unmap(struct irq_domain *domain,
					     unsigned int virq)
{
	(void)domain;
	irq_set_nested_thread(virq, 0);
	irq_set_chip_and_handler(virq, NULL, NULL);
	irq_set_chip_data(virq, NULL);
}

static int ums9117_sc2720_inta_domain_xlate(struct irq_domain *domain,
					    struct device_node *controller,
					    const u32 *intspec,
					    unsigned int intsize,
					    irq_hw_number_t *out_hwirq,
					    unsigned int *out_type)
{
	irq_hw_number_t source;

	(void)domain;
	(void)controller;
	if (intsize != 2)
		return -EINVAL;

	source = intspec[0];
	if (!ums9117_sc2720_inta_source_supported(source) ||
	    intspec[1] != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;

	*out_hwirq = source;
	*out_type = intspec[1];
	return 0;
}

static const struct irq_domain_ops ums9117_sc2720_inta_domain_ops = {
	.map = ums9117_sc2720_inta_domain_map,
	.unmap = ums9117_sc2720_inta_domain_unmap,
	.xlate = ums9117_sc2720_inta_domain_xlate,
};

static irqreturn_t ums9117_sc2720_inta_parent_thread(int irq, void *data)
{
	struct ums9117_sc2720_inta *inta = data;
	unsigned long pending;
	u16 status;
	u16 raw;
	u16 enable;
	u16 sync;
	unsigned int source;
	int ret;

	(void)irq;
	mutex_lock(&inta->lock);
	if (inta->failed)
		goto out_unlock;

	ret = ums9117_sc2720_inta_adi_read(SC2720_ANA_INT_STATUS, &status);
	if (ret)
		goto fail;
	ret = ums9117_sc2720_inta_adi_read(SC2720_ANA_INT_RAW, &raw);
	if (ret)
		goto fail;
	ret = ums9117_sc2720_inta_adi_read(SC2720_ANA_INT_ENABLE, &enable);
	if (ret)
		goto fail;
	ret = ums9117_sc2720_inta_adi_read(SC2720_ANA_INT_STATUS_SYNC, &sync);
	if (ret)
		goto fail;

	if (enable != inta->enabled ||
	    (status & enable & ~SC2720_INTA_SUPPORTED_MASK) ||
	    (raw & enable & ~SC2720_INTA_SUPPORTED_MASK) ||
	    (sync & enable & ~SC2720_INTA_SUPPORTED_MASK) ||
	    !(status & inta->enabled) || !(raw & inta->enabled)) {
		ret = -EPROTO;
		goto fail;
	}

	pending = status & raw & inta->enabled;
	if (!pending) {
		ret = -EPROTO;
		goto fail;
	}
	mutex_unlock(&inta->lock);

	/* Child handlers can sleep and acquire their own IRQ bus locks. */
	for_each_set_bit(source, &pending, SC2720_INTA_DOMAIN_SIZE) {
		unsigned int child_irq;

		child_irq = irq_find_mapping(inta->domain, source);
		if (!child_irq) {
			mutex_lock(&inta->lock);
			ums9117_sc2720_inta_fail_locked(inta, "child mapping",
							-ENXIO);
			mutex_unlock(&inta->lock);
			return IRQ_HANDLED;
		}
		handle_nested_irq(child_irq);
	}

	return IRQ_HANDLED;

fail:
	ums9117_sc2720_inta_fail_locked(inta, "parent IRQ", ret);
out_unlock:
	mutex_unlock(&inta->lock);
	return IRQ_HANDLED;
}

static int ums9117_sc2720_inta_validate_parent(struct ums9117_sc2720_inta *inta)
{
	struct irq_data *irq_data;

	irq_data = irq_get_irq_data(inta->parent_irq);
	if (!irq_data || irqd_to_hwirq(irq_data) != UMS9117_SC2720_INTA_HWIRQ ||
	    irqd_get_trigger_type(irq_data) != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;

	return 0;
}

static int ums9117_sc2720_inta_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ums9117_sc2720_inta *inta;
	int ret;

	inta = devm_kzalloc(dev, sizeof(*inta), GFP_KERNEL);
	if (!inta)
		return -ENOMEM;
	inta->dev = dev;
	mutex_init(&inta->lock);

	inta->parent_irq = platform_get_irq(pdev, 0);
	if (inta->parent_irq < 0)
		return dev_err_probe(
			dev, inta->parent_irq,
			"could not resolve SC2720 INTA parent IRQ\n");
	ret = ums9117_sc2720_inta_validate_parent(inta);
	if (ret)
		return dev_err_probe(
			dev, ret, "parent IRQ must be GIC SPI%u level-high\n",
			UMS9117_SC2720_INTA_SPI);

	ret = ums9117_sc2720_inta_read_baseline(inta);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"SC2720 INTA inherited state is not usable\n");

	inta->domain = irq_domain_create_linear(dev_fwnode(dev),
						SC2720_INTA_DOMAIN_SIZE,
						&ums9117_sc2720_inta_domain_ops,
						inta);
	if (!inta->domain)
		return dev_err_probe(dev, -ENOMEM,
				     "could not create SC2720 INTA domain\n");

	ret = request_threaded_irq(
		inta->parent_irq, NULL, ums9117_sc2720_inta_parent_thread,
		IRQF_TRIGGER_HIGH | IRQF_ONESHOT | IRQF_NO_AUTOEN,
		dev_name(dev), inta);
	if (ret)
		goto out_remove_domain;

	ret = ums9117_sc2720_inta_adi_write(SC2720_ANA_INT_ENABLE, 0);
	if (ret)
		goto out_free_irq;

	enable_irq(inta->parent_irq);
	inta->parent_enabled = true;
	inta->published = true;
	platform_set_drvdata(pdev, inta);
	dev_info(dev, "SC2720 INTA registered on SPI%u\n",
		 UMS9117_SC2720_INTA_SPI);
	return 0;

out_free_irq:
	/* Do not restore the inherited all-sources-enabled state on failure. */
	ums9117_sc2720_inta_adi_write(SC2720_ANA_INT_ENABLE, 0);
	free_irq(inta->parent_irq, inta);
out_remove_domain:
	irq_domain_remove(inta->domain);
	return dev_err_probe(dev, ret, "could not register SC2720 INTA\n");
}

static void ums9117_sc2720_inta_shutdown(struct platform_device *pdev)
{
	struct ums9117_sc2720_inta *inta = platform_get_drvdata(pdev);
	int ret;

	if (!inta || !inta->published)
		return;

	ums9117_sc2720_inta_disable_parent(inta);
	synchronize_irq(inta->parent_irq);
	mutex_lock(&inta->lock);
	inta->failed = true;
	inta->enabled = 0;
	ret = ums9117_sc2720_inta_adi_write(SC2720_ANA_INT_ENABLE, 0);
	if (ret)
		dev_err(inta->dev,
			"could not disable SC2720 analog interrupt sources: %d\n",
			ret);
	mutex_unlock(&inta->lock);
}

static const struct of_device_id ums9117_sc2720_inta_of_match[] = {
	{ .compatible = "sprd,ums9117-sc2720-inta" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_sc2720_inta_of_match);

static struct platform_driver ums9117_sc2720_inta_driver = {
	.probe = ums9117_sc2720_inta_probe,
	.shutdown = ums9117_sc2720_inta_shutdown,
	.driver = {
		.name = "ums9117-sc2720-inta",
		.of_match_table = ums9117_sc2720_inta_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ums9117_sc2720_inta_driver);

MODULE_DESCRIPTION("UMS9117 SC2720 analog interrupt aggregate");
MODULE_LICENSE("GPL");

#endif
