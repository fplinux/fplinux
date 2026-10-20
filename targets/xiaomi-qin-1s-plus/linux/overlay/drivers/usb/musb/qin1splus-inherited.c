// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qin1s+ inherited MUSB peripheral glue
 *
 * Copyright (c) 2016 Spreadtrum Co., Ltd.
 */

#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/usb/otg.h>

#include "musb_core.h"

#define QIN1SPLUS_MUSB_MMIO_BYTES 0x2000

static void read_fifo(struct musb_hw_ep *hw_ep, u16 len, u8 *dst)
{
	void __iomem *fifo = hw_ep->fifo;

	/* Keep FIFO reads word-wide and the final RAM copy within the request. */
	while (len) {
		__le32 word = cpu_to_le32(__raw_readl(fifo));
		u16 count = len < sizeof(word) ? len : sizeof(word);

		memcpy(dst, &word, count);
		dst += count;
		len -= count;
	}
}

static irqreturn_t core_irq(int irq, void *data)
{
	struct musb *musb = data;
	unsigned long flags;
	irqreturn_t ret = IRQ_NONE;
	u8 usb_mask;
	u16 tx_mask;
	u16 rx_mask;

	spin_lock_irqsave(&musb->lock, flags);

	usb_mask = musb_readb(musb->mregs, MUSB_INTRUSBE);
	tx_mask = musb_readw(musb->mregs, MUSB_INTRTXE);
	rx_mask = musb_readw(musb->mregs, MUSB_INTRRXE);
	musb->int_usb = musb_clearb(musb->mregs, MUSB_INTRUSB) & usb_mask;
	musb->int_tx = musb_clearw(musb->mregs, MUSB_INTRTX) & tx_mask;
	musb->int_rx = musb_clearw(musb->mregs, MUSB_INTRRX) & rx_mask;

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		ret = musb_interrupt(musb);

	spin_unlock_irqrestore(&musb->lock, flags);

	return ret;
}

static void disconnect(struct musb *musb)
{
	u8 power = musb_readb(musb->mregs, MUSB_POWER);

	musb_writeb(musb->mregs, MUSB_POWER, power & ~MUSB_POWER_SOFTCONN);
}

static int core_init(struct musb *musb)
{
	/* The RAM entry resets the peripheral before this driver binds. */
	musb->dyn_fifo = true;
	musb->isr = core_irq;

	return 0;
}

static int core_exit(struct musb *musb)
{
	disconnect(musb);

	return 0;
}

static const struct musb_fifo_cfg fifo_config[] = {
	/*
	 * Both composite function orders need three IN and two OUT FIFOs.
	 * Keep every IN FIFO large enough for a high-speed bulk endpoint.
	 */
	MUSB_EP_FIFO_SINGLE(1, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(1, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(3, FIFO_TX, 512),
};

static const struct musb_hdrc_config core_config = {
	.fifo_cfg = fifo_config,
	.fifo_cfg_size = ARRAY_SIZE(fifo_config),
	.num_eps = 16,
	.ram_bits = 13,
	.maximum_speed = USB_SPEED_HIGH,
};

static const struct musb_platform_ops core_ops = {
	.read_fifo = read_fifo,
	.init = core_init,
	.exit = core_exit,
	.disable = disconnect,
};

static const struct musb_hdrc_platform_data core_data = {
	.mode = MUSB_PERIPHERAL,
	.config = &core_config,
	.platform_ops = &core_ops,
};

static int qin1splus_musb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *core;
	struct resource *resource;
	int ret;

	if (usb_get_dr_mode(dev) != USB_DR_MODE_PERIPHERAL)
		return dev_err_probe(dev, -EINVAL,
				     "peripheral mode required\n");

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource_size(resource) != QIN1SPLUS_MUSB_MMIO_BYTES)
		return dev_err_probe(dev, -EINVAL,
				     "invalid MUSB register resource\n");

	ret = platform_get_irq_byname(pdev, "mc");
	if (ret < 0)
		return dev_err_probe(dev, ret, "missing MUSB core IRQ\n");

	core = platform_device_alloc("musb-hdrc", PLATFORM_DEVID_AUTO);
	if (!core)
		return -ENOMEM;

	core->dev.parent = dev;
	device_set_of_node_from_dev(&core->dev, dev);

	ret = platform_device_add_resources(core, pdev->resource,
					    pdev->num_resources);
	if (ret)
		goto put_core;

	ret = platform_device_add_data(core, &core_data, sizeof(core_data));
	if (ret)
		goto put_core;

	ret = platform_device_add(core);
	if (ret)
		goto put_core;

	platform_set_drvdata(pdev, core);

	return 0;

put_core:
	platform_device_put(core);
	return dev_err_probe(dev, ret, "failed to register MUSB core\n");
}

static void qin1splus_musb_remove(struct platform_device *pdev)
{
	struct platform_device *core = platform_get_drvdata(pdev);

	platform_device_unregister(core);
}

static const struct of_device_id qin1splus_musb_of_match[] = {
	{ .compatible = "fplinux,qin1splus-musb-inherited" },
	{}
};
MODULE_DEVICE_TABLE(of, qin1splus_musb_of_match);

static struct platform_driver qin1splus_musb_driver = {
	.probe = qin1splus_musb_probe,
	.remove = qin1splus_musb_remove,
	.driver = {
		.name = "musb-qin1splus-inherited",
		.of_match_table = qin1splus_musb_of_match,
	},
};
module_platform_driver(qin1splus_musb_driver);

MODULE_DESCRIPTION("Qin1s+ inherited MUSB peripheral glue");
MODULE_LICENSE("GPL");
