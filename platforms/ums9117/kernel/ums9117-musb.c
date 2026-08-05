// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal RAM-handoff glue for the MUSB HDRC in UMS9117/T117.
 *
 * The target bootstrap inherits an already powered USB PHY and core from the
 * BootROM/FDL path.  This glue intentionally does not guess cold-start clock,
 * reset, regulator or PHY programming.  It hands the proven MUSB register
 * window and stock-firmware IRQ to the mainline core in gadget-only PIO mode.
 */
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/usb/musb.h>

#include "musb_core.h"

#define UMS9117_DMA_MASK_STATUS 0x100cu
#define UMS9117_DMA_CHANNEL(n) (0x1c00u + ((n) - 1u) * 0x20u)
#define UMS9117_DMA_PAUSE 0x00u
#define UMS9117_DMA_CFG 0x04u
#define UMS9117_DMA_INTR 0x08u
#define UMS9117_DMA_LLIST_PTR 0x14u
#define UMS9117_DMA_INTR_CLEAR GENMASK(28, 24)
#define UMS9117_DMA_CHN_EN BIT(0)
#define UMS9117_DMA_CLEAR_INT_EN BIT(5)
#define UMS9117_DMA_CHN_CLR BIT(15)
#define UMS9117_DMA_CLEAR_STATUS BIT(21)
#define UMS9117_DMA_CH5_STATUS BIT(4)
#define UMS9117_DMA_CH21_STATUS BIT(20)
#define UMS9117_DMA_OWNED_STATUS \
	(UMS9117_DMA_CH5_STATUS | UMS9117_DMA_CH21_STATUS)

struct ums9117_musb_glue {
	struct platform_device *musb;
	struct resource resources[2];
	atomic_t irq_count;
	bool irq_disabled;
};

/*
 * The UMS9117 FIFO accepts 32-bit reads reliably.  Narrow reads can return a
 * stale or neighbouring byte for the final 1 to 3 bytes of an OUT packet.
 * Read the tail as one word and copy only the bytes reported by RXCOUNT.
 */
static void ums9117_musb_read_fifo32(struct musb_hw_ep *hw_ep, u16 len, u8 *dst)
{
	void __iomem *fifo = hw_ep->fifo;

	if (len >= sizeof(u32)) {
		unsigned int words = len / sizeof(u32);

		ioread32_rep(fifo, dst, words);
		dst += words * sizeof(u32);
		len %= sizeof(u32);
	}

	if (len) {
		u32 tail = musb_readl(fifo, 0);

		memcpy(dst, &tail, len);
	}
}

static bool ums9117_quiesce_dma_channel(void __iomem *base,
					unsigned int channel,
					unsigned int poll_limit)
{
	void __iomem *regs = base + UMS9117_DMA_CHANNEL(channel);
	u32 cfg;
	u32 intr;
	u32 pause;
	unsigned int polls;

	cfg = readl_relaxed(regs + UMS9117_DMA_CFG);
	if (!(cfg & UMS9117_DMA_CHN_EN)) {
		writel_relaxed(0, regs + UMS9117_DMA_LLIST_PTR);
		writel_relaxed(0, regs + UMS9117_DMA_PAUSE);
		/*
		 * The stores above are relaxed: drain them before returning
		 * "quiesced", so the engine drops its descriptor pointer into
		 * loader RAM before the caller may treat that RAM as
		 * reusable.
		 */
		wmb();
		return true;
	}

	intr = readl_relaxed(regs + UMS9117_DMA_INTR);
	writel_relaxed(intr | UMS9117_DMA_CLEAR_INT_EN,
		       regs + UMS9117_DMA_INTR);
	pause = readl_relaxed(regs + UMS9117_DMA_PAUSE);
	writel_relaxed(pause | UMS9117_DMA_CHN_CLR, regs + UMS9117_DMA_PAUSE);
	/*
	 * CHN_CLR is a request the channel acknowledges through
	 * CLEAR_STATUS. Post the relaxed request before the bounded relaxed
	 * poll below, or the poll budget can expire while the request still
	 * sits in the CPU write buffer, and the spurious "did not clear"
	 * result escalates to the sticky interrupt fail-safe.
	 */
	wmb();
	for (polls = 0; polls < poll_limit; ++polls) {
		intr = readl_relaxed(regs + UMS9117_DMA_INTR);
		if (intr & UMS9117_DMA_CLEAR_STATUS)
			break;
		cpu_relax();
	}
	writel_relaxed(intr | UMS9117_DMA_INTR_CLEAR, regs + UMS9117_DMA_INTR);
	writel_relaxed(0, regs + UMS9117_DMA_CFG);
	writel_relaxed(0, regs + UMS9117_DMA_LLIST_PTR);
	writel_relaxed(0, regs + UMS9117_DMA_PAUSE);
	/*
	 * Order the disable and pointer-clear stores before the CFG
	 * readback in the return value: a stale readback either fails a
	 * quiesced channel (USB stays disabled) or passes one still armed
	 * over reusable loader RAM.
	 */
	wmb();
	return (intr & UMS9117_DMA_CLEAR_STATUS) &&
	       !(readl_relaxed(regs + UMS9117_DMA_CFG) & UMS9117_DMA_CHN_EN);
}

static irqreturn_t ums9117_musb_interrupt(int irq, void *data)
{
	struct musb *musb = data;
	struct ums9117_musb_glue *glue =
		dev_get_drvdata(musb->controller->parent);
	unsigned long flags;
	irqreturn_t result = IRQ_NONE;
	u32 legacy_dma;
	u16 mask16;
	u8 mask8;
	int count;
	bool dma5 = true;
	bool dma21 = true;
	u32 remaining_dma;

	spin_lock_irqsave(&musb->lock, flags);
	mask8 = musb_readb(musb->mregs, MUSB_INTRUSBE);
	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB) & mask8;
	mask16 = musb_readw(musb->mregs, MUSB_INTRTXE);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX) & mask16;
	mask16 = musb_readw(musb->mregs, MUSB_INTRRXE);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX) & mask16;

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		result = musb_interrupt(musb);

	/*
	 * PIO is the only supported Linux mode.  If a status bit survived the
	 * bootstrap handoff, clear the two channels fpdoom used rather than
	 * allowing an unowned DMA source to hold SPI 55 asserted.
	 */
	legacy_dma = musb_readl(musb->mregs, UMS9117_DMA_MASK_STATUS);
	if (legacy_dma & UMS9117_DMA_CH5_STATUS)
		dma5 = ums9117_quiesce_dma_channel(musb->mregs, 5, 1024);
	if (legacy_dma & UMS9117_DMA_CH21_STATUS)
		dma21 = ums9117_quiesce_dma_channel(musb->mregs, 21, 1024);
	remaining_dma = musb_readl(musb->mregs, UMS9117_DMA_MASK_STATUS);
	if (legacy_dma || remaining_dma)
		result = IRQ_HANDLED;
	if ((!dma5 || !dma21 || remaining_dma) && !glue->irq_disabled) {
		/*
		 * Do not touch unknown channels.  SPI 55 is level-high, so an
		 * unowned or uncleared source would otherwise cause an IRQ storm.
		 * Stop the gadget path and leave exact masks in dmesg.
		 */
		musb_writeb(musb->mregs, MUSB_INTRUSBE, 0);
		musb_writew(musb->mregs, MUSB_INTRTXE, 0);
		musb_writew(musb->mregs, MUSB_INTRRXE, 0);
		musb_writeb(musb->mregs, MUSB_POWER,
			    musb_readb(musb->mregs, MUSB_POWER) &
				    ~MUSB_POWER_SOFTCONN);
		glue->irq_disabled = true;
		dev_err(musb->controller,
			"disabling SPI 55: DMA before=%08x after=%08x known-clear=%u/%u\n",
			legacy_dma, remaining_dma, dma5, dma21);
		disable_irq_nosync(irq);
	}
	spin_unlock_irqrestore(&musb->lock, flags);

	count = atomic_inc_return(&glue->irq_count);
	if (count == 1)
		dev_info(musb->controller,
			 "first SPI 55: usb=%02x tx=%04x rx=%04x dma=%08x\n",
			 musb->int_usb, musb->int_tx, musb->int_rx, legacy_dma);
	return result;
}

static int ums9117_musb_init(struct musb *musb)
{
	bool dma5;
	bool dma21;
	u32 remaining_dma;

	/*
	 * The block exposes flat endpoint registers at 0x100 + ep * 0x10,
	 * while dynamic FIFO sizing is selected through INDEX.  Therefore no
	 * MUSB_INDEXED_EP quirk is used.
	 */
	musb->dyn_fifo = true;
	musb->isr = ums9117_musb_interrupt;
	dma5 = ums9117_quiesce_dma_channel(musb->mregs, 5, 1000000);
	dma21 = ums9117_quiesce_dma_channel(musb->mregs, 21, 1000000);
	remaining_dma = musb_readl(musb->mregs, UMS9117_DMA_MASK_STATUS);
	if (!dma5 || !dma21 || remaining_dma) {
		dev_err(musb->controller,
			"refusing MUSB handoff: DMA5=%u DMA21=%u mask=%08x\n",
			dma5, dma21, remaining_dma);
		return -EBUSY;
	}
	return 0;
}

static int ums9117_musb_exit(struct musb *musb)
{
	return 0;
}

static const struct musb_platform_ops ums9117_musb_ops = {
	.init = ums9117_musb_init,
	.exit = ums9117_musb_exit,
	.read_fifo = ums9117_musb_read_fifo32,
};

static const struct musb_fifo_cfg ums9117_fifo_cfg[] = {
	MUSB_EP_FIFO_SINGLE(1, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(1, FIFO_RX, 512),
};

static const struct musb_hdrc_config ums9117_musb_config = {
	.fifo_cfg = ums9117_fifo_cfg,
	.fifo_cfg_size = ARRAY_SIZE(ums9117_fifo_cfg),
	.multipoint = false,
	.dyn_fifo = true,
	.num_eps = 16,
	.ram_bits = 13,
	.maximum_speed = USB_SPEED_HIGH,
};

static const struct musb_hdrc_platform_data ums9117_musb_pdata = {
	.mode = MUSB_PERIPHERAL,
	.config = &ums9117_musb_config,
	.platform_ops = &ums9117_musb_ops,
};

static int ums9117_musb_probe(struct platform_device *pdev)
{
	struct ums9117_musb_glue *glue;
	struct resource *memory;
	int irq;
	struct platform_device_info info = {
		.name = "musb-hdrc",
		.id = PLATFORM_DEVID_AUTO,
		.parent = &pdev->dev,
		.data = &ums9117_musb_pdata,
		.size_data = sizeof(ums9117_musb_pdata),
		.dma_mask = DMA_BIT_MASK(32),
	};

	memory = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!memory)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "missing MUSB register resource\n");
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	glue = devm_kzalloc(&pdev->dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;
	atomic_set(&glue->irq_count, 0);
	glue->resources[0] = *memory;
	glue->resources[1] = (struct resource){
		.start = irq,
		.end = irq,
		.name = "mc",
		.flags = IORESOURCE_IRQ,
	};
	info.res = glue->resources;
	info.num_res = ARRAY_SIZE(glue->resources);
	platform_set_drvdata(pdev, glue);

	glue->musb = platform_device_register_full(&info);
	if (IS_ERR(glue->musb))
		return dev_err_probe(&pdev->dev, PTR_ERR(glue->musb),
				     "could not register MUSB core\n");

	dev_info(&pdev->dev,
		 "registered inherited-state MUSB gadget (PIO only, SPI 55)\n");
	return 0;
}

static void ums9117_musb_remove(struct platform_device *pdev)
{
	struct ums9117_musb_glue *glue = platform_get_drvdata(pdev);

	platform_device_unregister(glue->musb);
}

static const struct of_device_id ums9117_musb_of_match[] = {
	{ .compatible = "fplinux,ums9117-musb-inherited" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_musb_of_match);

static struct platform_driver ums9117_musb_driver = {
	.probe = ums9117_musb_probe,
	.remove = ums9117_musb_remove,
	.driver = {
		.name = "ums9117-musb-inherited",
		.of_match_table = ums9117_musb_of_match,
	},
};
module_platform_driver(ums9117_musb_driver);

MODULE_DESCRIPTION("UMS9117 inherited-state MUSB gadget glue");
MODULE_LICENSE("GPL");
