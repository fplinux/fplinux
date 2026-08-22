// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/usb/musb.h>

#include "musb_core.h"

#define UMS9117_MUSB_AP_AHB_EB 0x0000U
#define UMS9117_MUSB_AP_AHB_RST 0x0004U
#define UMS9117_MUSB_AP_AHB_SET 0x1000U
#define UMS9117_MUSB_AP_AHB_CLEAR 0x2000U
#define UMS9117_MUSB_AP_AHB_PHY_TEST 0x3024U
#define UMS9117_MUSB_AP_AHB_PHY_CTRL 0x3028U
#define UMS9117_MUSB_AP_AHB_CTRL1 0x3030U
#define UMS9117_MUSB_AP_AHB_BYTES 0x4000U

#define UMS9117_MUSB_ANLG_PHY_G8_TRIMMING 0x0018U
#define UMS9117_MUSB_ANLG_PHY_G8_SELECT 0x0024U
#define UMS9117_MUSB_ANLG_PHY_G8_SET 0x1000U
#define UMS9117_MUSB_ANLG_PHY_G8_CLEAR 0x2000U
#define UMS9117_MUSB_ANLG_PHY_G8_BYTES 0x3000U

#define UMS9117_MUSB_ANLG_PHY_TOP_ISOLATION 0x0010U
#define UMS9117_MUSB_ANLG_PHY_TOP_SET 0x1000U
#define UMS9117_MUSB_ANLG_PHY_TOP_CLEAR 0x2000U
#define UMS9117_MUSB_ANLG_PHY_TOP_BYTES 0x3000U

#define UMS9117_MUSB_AON_APB_EB2 0x00b0U
#define UMS9117_MUSB_AON_APB_PWR_CTRL 0x0024U

#define UMS9117_MUSB_AP_AHB_OTG_EB BIT(4)
#define UMS9117_MUSB_AP_AHB_OTG_RESET (BIT(4) | BIT(5) | BIT(6))
#define UMS9117_MUSB_AON_APB_ANALOG_EB BIT(11)
#define UMS9117_MUSB_ANLG_PHY_G8_SELECT_USB20 BIT(9)
#define UMS9117_MUSB_ANLG_PHY_G8_TUNE_HSAMP BIT(27)
#define UMS9117_MUSB_AP_AHB_VBUS_VALID (BIT(24) | BIT(22))
#define UMS9117_MUSB_AP_AHB_UTMI_16BIT (BIT(30) | BIT(29))
#define UMS9117_MUSB_AP_AHB_CTRL1_MASK GENMASK(15, 0)
#define UMS9117_MUSB_AP_AHB_CTRL1_VALUE 0x17c0U
#define UMS9117_MUSB_AON_APB_USB_PHY_PD_S BIT(17)
#define UMS9117_MUSB_AON_APB_USB_PHY_PD_L BIT(16)
#define UMS9117_MUSB_ANLG_PHY_TOP_USB20_ISOLATION BIT(0)

#define UMS9117_MUSB_CLOCK_SETTLE_MS 10U
#define UMS9117_MUSB_RESET_MS 10U
#define UMS9117_MUSB_PHY_SWITCH_MIN_US 1000U
#define UMS9117_MUSB_PHY_SWITCH_MAX_US 1200U
#define UMS9117_MUSB_LDO_PRE_CYCLE_MS 5U
#define UMS9117_MUSB_LDO_OFF_MS 10U

#define UMS9117_MUSB_DMA_MASK_STATUS 0x100cU
#define UMS9117_MUSB_DMA_CHANNEL(n) (0x1c00U + ((n) - 1U) * 0x20U)
#define UMS9117_MUSB_DMA_PAUSE 0x00U
#define UMS9117_MUSB_DMA_CFG 0x04U
#define UMS9117_MUSB_DMA_INTR 0x08U
#define UMS9117_MUSB_DMA_LLIST_PTR 0x14U
#define UMS9117_MUSB_DMA_INTR_CLEAR_MASK GENMASK(28, 24)
#define UMS9117_MUSB_DMA_CHANNEL_ENABLE BIT(0)
#define UMS9117_MUSB_DMA_CLEAR_INTERRUPT_ENABLE BIT(5)
#define UMS9117_MUSB_DMA_CHANNEL_CLEAR BIT(15)
#define UMS9117_MUSB_DMA_CLEAR_STATUS BIT(21)
#define UMS9117_MUSB_DMA_CHANNEL5_STATUS BIT(4)
#define UMS9117_MUSB_DMA_CHANNEL21_STATUS BIT(20)

struct ums9117_musb_match_data {
	bool cold_owned;
};

struct ums9117_musb_glue {
	struct device *dev;
	struct platform_device *musb;
	struct resource resources[2];
	struct regmap *aon_apb;
	void __iomem *ap_ahb;
	void __iomem *anlg_phy_g8;
	void __iomem *anlg_phy_top;
	struct regulator *vddusb;
	atomic_t irq_count;
	bool cold_owned;
	bool vddusb_enabled;
	bool vddusb_force_off;
	bool irq_disabled;
};

static void __iomem *ums9117_musb_ioremap_shared(struct platform_device *pdev,
						 const char *name,
						 resource_size_t minimum_size)
{
	struct resource *resource;
	void __iomem *base;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource_size(resource) < minimum_size)
		return IOMEM_ERR_PTR(-EINVAL);
	base = devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
	return base ? base : IOMEM_ERR_PTR(-ENOMEM);
}

static int ums9117_musb_alias_update(void __iomem *base, u32 offset,
				     u32 set_alias, u32 clear_alias, u32 mask,
				     u32 value)
{
	u32 readback;

	if (value & mask)
		writel(value & mask, base + set_alias + offset);
	if (~value & mask)
		writel(~value & mask, base + clear_alias + offset);
	/* Post both write-only alias transactions before checking state. */
	wmb();
	readback = readl(base + offset);
	return (readback & mask) == (value & mask) ? 0 : -EIO;
}

static int ums9117_musb_update_bits(void __iomem *base, u32 offset, u32 mask,
				    u32 value)
{
	u32 readback;
	u32 current_value;

	current_value = readl(base + offset);
	writel((current_value & ~mask) | (value & mask), base + offset);
	readback = readl(base + offset);
	return (readback & mask) == (value & mask) ? 0 : -EIO;
}

static int ums9117_musb_regmap_update_bits(struct regmap *regmap, u32 offset,
					   u32 mask, u32 value)
{
	unsigned int readback;
	int ret;

	ret = regmap_update_bits(regmap, offset, mask, value);
	if (ret)
		return ret;
	ret = regmap_read(regmap, offset, &readback);
	if (ret)
		return ret;
	return (readback & mask) == (value & mask) ? 0 : -EIO;
}

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

static bool ums9117_musb_quiesce_dma_channel(void __iomem *base,
					     unsigned int channel,
					     unsigned int poll_attempts)
{
	void __iomem *regs = base + UMS9117_MUSB_DMA_CHANNEL(channel);
	u32 cfg;
	u32 intr;
	u32 pause;
	unsigned int polls;

	cfg = readl_relaxed(regs + UMS9117_MUSB_DMA_CFG);
	if (!(cfg & UMS9117_MUSB_DMA_CHANNEL_ENABLE)) {
		writel_relaxed(0, regs + UMS9117_MUSB_DMA_LLIST_PTR);
		writel_relaxed(0, regs + UMS9117_MUSB_DMA_PAUSE);
		/*
		 * The stores above are relaxed: drain them before returning
		 * "quiesced", so the engine drops its descriptor pointer into
		 * loader RAM before the caller may treat that RAM as
		 * reusable.
		 */
		wmb();
		return true;
	}

	intr = readl_relaxed(regs + UMS9117_MUSB_DMA_INTR);
	writel_relaxed(intr | UMS9117_MUSB_DMA_CLEAR_INTERRUPT_ENABLE,
		       regs + UMS9117_MUSB_DMA_INTR);
	pause = readl_relaxed(regs + UMS9117_MUSB_DMA_PAUSE);
	writel_relaxed(pause | UMS9117_MUSB_DMA_CHANNEL_CLEAR,
		       regs + UMS9117_MUSB_DMA_PAUSE);
	/*
	 * CHN_CLR is a request the channel acknowledges through
	 * CLEAR_STATUS. Post the relaxed request before the bounded relaxed
	 * poll below, or the poll budget can expire while the request still
	 * sits in the CPU write buffer, and the spurious "did not clear"
	 * result escalates to the sticky interrupt fail-safe.
	 */
	wmb();
	for (polls = 0; polls < poll_attempts; ++polls) {
		intr = readl_relaxed(regs + UMS9117_MUSB_DMA_INTR);
		if (intr & UMS9117_MUSB_DMA_CLEAR_STATUS)
			break;
		cpu_relax();
	}
	writel_relaxed(intr | UMS9117_MUSB_DMA_INTR_CLEAR_MASK,
		       regs + UMS9117_MUSB_DMA_INTR);
	writel_relaxed(0, regs + UMS9117_MUSB_DMA_CFG);
	writel_relaxed(0, regs + UMS9117_MUSB_DMA_LLIST_PTR);
	writel_relaxed(0, regs + UMS9117_MUSB_DMA_PAUSE);
	/*
	 * Order the disable and pointer-clear stores before the CFG
	 * readback in the return value: a stale readback either fails a
	 * quiesced channel (USB stays disabled) or passes one still armed
	 * over reusable loader RAM.
	 */
	wmb();
	return (intr & UMS9117_MUSB_DMA_CLEAR_STATUS) &&
	       !(readl_relaxed(regs + UMS9117_MUSB_DMA_CFG) &
		 UMS9117_MUSB_DMA_CHANNEL_ENABLE);
}

static int ums9117_musb_quiesce_dma(struct musb *musb, const char *operation)
{
	bool dma5;
	bool dma21;
	u32 remaining_dma;

	dma5 = ums9117_musb_quiesce_dma_channel(musb->mregs, 5, 1000000);
	dma21 = ums9117_musb_quiesce_dma_channel(musb->mregs, 21, 1000000);
	remaining_dma = musb_readl(musb->mregs, UMS9117_MUSB_DMA_MASK_STATUS);
	if (dma5 && dma21 && !remaining_dma)
		return 0;

	dev_err(musb->controller, "%s: DMA5=%u DMA21=%u mask=%08x\n", operation,
		dma5, dma21, remaining_dma);
	return -EBUSY;
}

static void ums9117_musb_keep_first_error(struct device *dev, int *result,
					  int error, const char *stage)
{
	if (error)
		dev_err(dev, "cold USB shutdown failed at %s: %pe\n", stage,
			ERR_PTR(error));
	if (error && !*result)
		*result = error;
}

static int ums9117_musb_enable_analog_gate(struct ums9117_musb_glue *glue)
{
	return ums9117_musb_regmap_update_bits(glue->aon_apb,
					       UMS9117_MUSB_AON_APB_EB2,
					       UMS9117_MUSB_AON_APB_ANALOG_EB,
					       UMS9117_MUSB_AON_APB_ANALOG_EB);
}

static int ums9117_musb_isolate_phy(struct ums9117_musb_glue *glue)
{
	return ums9117_musb_alias_update(
		glue->anlg_phy_top, UMS9117_MUSB_ANLG_PHY_TOP_ISOLATION,
		UMS9117_MUSB_ANLG_PHY_TOP_SET, UMS9117_MUSB_ANLG_PHY_TOP_CLEAR,
		UMS9117_MUSB_ANLG_PHY_TOP_USB20_ISOLATION,
		UMS9117_MUSB_ANLG_PHY_TOP_USB20_ISOLATION);
}

static int ums9117_musb_disable_vddusb(struct ums9117_musb_glue *glue)
{
	int ret;

	if (glue->vddusb_enabled) {
		ret = regulator_disable(glue->vddusb);
		if (!ret) {
			glue->vddusb_enabled = false;
			glue->vddusb_force_off = false;
		}
		return ret;
	}
	if (!glue->vddusb_force_off)
		return 0;

	ret = regulator_hardware_enable(glue->vddusb, false);
	if (!ret)
		glue->vddusb_force_off = false;
	return ret;
}

static int ums9117_musb_cold_shutdown(struct musb *musb)
{
	struct ums9117_musb_glue *glue =
		dev_get_drvdata(musb->controller->parent);
	bool analog_gate_ready = false;
	bool phy_isolated = false;
	bool gate_enabled;
	int ret = 0;
	int step;

	/*
	 * musb_remove() masks the generic core before calling platform exit, but
	 * the IRQ action is freed only after this callback. Drain any handler
	 * which may still be using the powered register window before resetting
	 * the block.
	 */
	if (musb->nIrq >= 0)
		synchronize_irq(musb->nIrq);

	gate_enabled = readl(glue->ap_ahb + UMS9117_MUSB_AP_AHB_EB) &
		       UMS9117_MUSB_AP_AHB_OTG_EB;
	if (gate_enabled) {
		step = ums9117_musb_quiesce_dma(
			musb, "USB shutdown DMA did not quiesce");
		ums9117_musb_keep_first_error(musb->controller, &ret, step,
					      "DMA quiesce");
	}

	step = ums9117_musb_alias_update(glue->ap_ahb, UMS9117_MUSB_AP_AHB_RST,
					 UMS9117_MUSB_AP_AHB_SET,
					 UMS9117_MUSB_AP_AHB_CLEAR,
					 UMS9117_MUSB_AP_AHB_OTG_RESET,
					 UMS9117_MUSB_AP_AHB_OTG_RESET);
	ums9117_musb_keep_first_error(musb->controller, &ret, step,
				      "reset assertion");

	/*
	 * The fail-safe ISR disables this level IRQ without balancing it. Once
	 * reset has removed the source, balance the descriptor before musb_free()
	 * releases the action so a later bind does not inherit a disabled line.
	 */
	if (glue->irq_disabled && musb->nIrq >= 0) {
		enable_irq(musb->nIrq);
		glue->irq_disabled = false;
		synchronize_irq(musb->nIrq);
	}

	/* G8 and PHY-TOP are safe to access only after this shared gate reads on. */
	step = ums9117_musb_enable_analog_gate(glue);
	if (!step)
		analog_gate_ready = true;
	ums9117_musb_keep_first_error(musb->controller, &ret, step,
				      "analog APB gate");

	step = ums9117_musb_update_bits(glue->ap_ahb,
					UMS9117_MUSB_AP_AHB_PHY_TEST,
					UMS9117_MUSB_AP_AHB_VBUS_VALID, 0);
	ums9117_musb_keep_first_error(musb->controller, &ret, step,
				      "VBUS-valid override clear");
	step = ums9117_musb_update_bits(glue->ap_ahb,
					UMS9117_MUSB_AP_AHB_PHY_CTRL,
					UMS9117_MUSB_AP_AHB_UTMI_16BIT, 0);
	ums9117_musb_keep_first_error(musb->controller, &ret, step,
				      "UTMI-width clear");
	step = ums9117_musb_update_bits(glue->ap_ahb, UMS9117_MUSB_AP_AHB_CTRL1,
					UMS9117_MUSB_AP_AHB_CTRL1_MASK, 0);
	ums9117_musb_keep_first_error(musb->controller, &ret, step,
				      "control-word clear");
	if (analog_gate_ready) {
		step = ums9117_musb_update_bits(
			glue->anlg_phy_g8, UMS9117_MUSB_ANLG_PHY_G8_TRIMMING,
			UMS9117_MUSB_ANLG_PHY_G8_TUNE_HSAMP, 0);
		ums9117_musb_keep_first_error(musb->controller, &ret, step,
					      "PHY trim clear");
		step = ums9117_musb_alias_update(
			glue->anlg_phy_g8, UMS9117_MUSB_ANLG_PHY_G8_SELECT,
			UMS9117_MUSB_ANLG_PHY_G8_SET,
			UMS9117_MUSB_ANLG_PHY_G8_CLEAR,
			UMS9117_MUSB_ANLG_PHY_G8_SELECT_USB20, 0);
		ums9117_musb_keep_first_error(musb->controller, &ret, step,
					      "PHY select clear");

		/* Isolate the macro before removing either of its power sources. */
		step = ums9117_musb_isolate_phy(glue);
		if (!step)
			phy_isolated = true;
		ums9117_musb_keep_first_error(musb->controller, &ret, step,
					      "PHY isolation");
	}

	if (phy_isolated) {
		step = ums9117_musb_regmap_update_bits(
			glue->aon_apb, UMS9117_MUSB_AON_APB_PWR_CTRL,
			UMS9117_MUSB_AON_APB_USB_PHY_PD_S |
				UMS9117_MUSB_AON_APB_USB_PHY_PD_L,
			UMS9117_MUSB_AON_APB_USB_PHY_PD_S |
				UMS9117_MUSB_AON_APB_USB_PHY_PD_L);
		ums9117_musb_keep_first_error(musb->controller, &ret, step,
					      "PHY power-down");

		step = ums9117_musb_disable_vddusb(glue);
		ums9117_musb_keep_first_error(musb->controller, &ret, step,
					      "vddusb disable");
	}

	step = ums9117_musb_alias_update(glue->ap_ahb, UMS9117_MUSB_AP_AHB_EB,
					 UMS9117_MUSB_AP_AHB_SET,
					 UMS9117_MUSB_AP_AHB_CLEAR,
					 UMS9117_MUSB_AP_AHB_OTG_EB, 0);
	ums9117_musb_keep_first_error(musb->controller, &ret, step,
				      "OTG clock gate clear");

	return ret;
}

static int ums9117_musb_cold_start(struct musb *musb)
{
	struct ums9117_musb_glue *glue =
		dev_get_drvdata(musb->controller->parent);
	const char *stage = "OTG clock gate";
	unsigned int aon_power;
	u32 trim_after;
	u32 trim_before;
	int unwind;
	int ret;

	ret = ums9117_musb_alias_update(glue->ap_ahb, UMS9117_MUSB_AP_AHB_EB,
					UMS9117_MUSB_AP_AHB_SET,
					UMS9117_MUSB_AP_AHB_CLEAR,
					UMS9117_MUSB_AP_AHB_OTG_EB,
					UMS9117_MUSB_AP_AHB_OTG_EB);
	if (ret)
		goto fail;
	msleep(UMS9117_MUSB_CLOCK_SETTLE_MS);

	stage = "OTG reset assertion";
	ret = ums9117_musb_alias_update(glue->ap_ahb, UMS9117_MUSB_AP_AHB_RST,
					UMS9117_MUSB_AP_AHB_SET,
					UMS9117_MUSB_AP_AHB_CLEAR,
					UMS9117_MUSB_AP_AHB_OTG_RESET,
					UMS9117_MUSB_AP_AHB_OTG_RESET);
	if (ret)
		goto fail;
	msleep(UMS9117_MUSB_RESET_MS);
	stage = "OTG reset deassertion";
	ret = ums9117_musb_alias_update(glue->ap_ahb, UMS9117_MUSB_AP_AHB_RST,
					UMS9117_MUSB_AP_AHB_SET,
					UMS9117_MUSB_AP_AHB_CLEAR,
					UMS9117_MUSB_AP_AHB_OTG_RESET, 0);
	if (ret)
		goto fail;

	stage = "loader DMA quiesce";
	ret = ums9117_musb_quiesce_dma(musb, "refusing cold MUSB ownership");
	if (ret)
		goto fail;

	/* This is a shared analog bus gate; USB acquires it but never clears it. */
	stage = "analog APB gate";
	ret = ums9117_musb_regmap_update_bits(glue->aon_apb,
					      UMS9117_MUSB_AON_APB_EB2,
					      UMS9117_MUSB_AON_APB_ANALOG_EB,
					      UMS9117_MUSB_AON_APB_ANALOG_EB);
	if (ret)
		goto fail;

	stage = "USB PHY select";
	ret = ums9117_musb_alias_update(glue->anlg_phy_g8,
					UMS9117_MUSB_ANLG_PHY_G8_SELECT,
					UMS9117_MUSB_ANLG_PHY_G8_SET,
					UMS9117_MUSB_ANLG_PHY_G8_CLEAR,
					UMS9117_MUSB_ANLG_PHY_G8_SELECT_USB20,
					UMS9117_MUSB_ANLG_PHY_G8_SELECT_USB20);
	if (ret)
		goto fail;

	/*
	 * T127's normal cold path selects bit 27, while the fitted test-mode
	 * path proves direct read-modify-write access to this base register.
	 * Bit 28 belongs to that separate electrical test mode and is preserved.
	 */
	stage = "USB PHY HS amplitude trim";
	trim_before =
		readl(glue->anlg_phy_g8 + UMS9117_MUSB_ANLG_PHY_G8_TRIMMING);
	ret = ums9117_musb_update_bits(glue->anlg_phy_g8,
				       UMS9117_MUSB_ANLG_PHY_G8_TRIMMING,
				       UMS9117_MUSB_ANLG_PHY_G8_TUNE_HSAMP,
				       UMS9117_MUSB_ANLG_PHY_G8_TUNE_HSAMP);
	trim_after =
		readl(glue->anlg_phy_g8 + UMS9117_MUSB_ANLG_PHY_G8_TRIMMING);
	if (ret)
		dev_err(musb->controller,
			"USB PHY HS amplitude trim readback before=%08x after=%08x\n",
			trim_before, trim_after);
	if (ret)
		goto fail;

	stage = "USB VBUS-valid override";
	ret = ums9117_musb_update_bits(glue->ap_ahb,
				       UMS9117_MUSB_AP_AHB_PHY_TEST,
				       UMS9117_MUSB_AP_AHB_VBUS_VALID,
				       UMS9117_MUSB_AP_AHB_VBUS_VALID);
	if (ret)
		goto fail;
	stage = "USB UTMI width";
	ret = ums9117_musb_update_bits(glue->ap_ahb,
				       UMS9117_MUSB_AP_AHB_PHY_CTRL,
				       UMS9117_MUSB_AP_AHB_UTMI_16BIT,
				       UMS9117_MUSB_AP_AHB_UTMI_16BIT);
	if (ret)
		goto fail;
	stage = "USB control word";
	ret = ums9117_musb_update_bits(glue->ap_ahb, UMS9117_MUSB_AP_AHB_CTRL1,
				       UMS9117_MUSB_AP_AHB_CTRL1_MASK,
				       UMS9117_MUSB_AP_AHB_CTRL1_VALUE);
	if (ret)
		goto fail;

	stage = "USB PHY small power switch";
	ret = ums9117_musb_regmap_update_bits(glue->aon_apb,
					      UMS9117_MUSB_AON_APB_PWR_CTRL,
					      UMS9117_MUSB_AON_APB_USB_PHY_PD_S,
					      0);
	if (ret)
		goto fail;
	usleep_range(UMS9117_MUSB_PHY_SWITCH_MIN_US,
		     UMS9117_MUSB_PHY_SWITCH_MAX_US);
	stage = "USB PHY large power switch";
	ret = ums9117_musb_regmap_update_bits(glue->aon_apb,
					      UMS9117_MUSB_AON_APB_PWR_CTRL,
					      UMS9117_MUSB_AON_APB_USB_PHY_PD_L,
					      0);
	if (ret)
		goto fail;
	stage = "USB PHY isolation";
	ret = ums9117_musb_alias_update(
		glue->anlg_phy_top, UMS9117_MUSB_ANLG_PHY_TOP_ISOLATION,
		UMS9117_MUSB_ANLG_PHY_TOP_SET, UMS9117_MUSB_ANLG_PHY_TOP_CLEAR,
		UMS9117_MUSB_ANLG_PHY_TOP_USB20_ISOLATION, 0);
	if (ret)
		goto fail;

	msleep(UMS9117_MUSB_LDO_PRE_CYCLE_MS);
	if (glue->vddusb_enabled) {
		stage = "vddusb disable";
		ret = regulator_disable(glue->vddusb);
		if (ret)
			goto fail;
		glue->vddusb_enabled = false;
	}
	msleep(UMS9117_MUSB_LDO_OFF_MS);
	stage = "vddusb enable";
	glue->vddusb_force_off = true;
	ret = regulator_enable(glue->vddusb);
	if (ret)
		goto fail;
	glue->vddusb_enabled = true;
	glue->vddusb_force_off = false;

	stage = "final AON power readback";
	ret = regmap_read(glue->aon_apb, UMS9117_MUSB_AON_APB_PWR_CTRL,
			  &aon_power);
	if (ret)
		goto fail;
	if (aon_power & (UMS9117_MUSB_AON_APB_USB_PHY_PD_S |
			 UMS9117_MUSB_AON_APB_USB_PHY_PD_L)) {
		ret = -EIO;
		goto fail;
	}
	stage = "final vddusb readback";
	ret = regulator_is_enabled(glue->vddusb);
	if (ret <= 0) {
		if (!ret)
			ret = -EIO;
		goto fail;
	}

	dev_info(musb->controller, "cold USB initialized\n");
	return 0;

fail:
	dev_err(musb->controller, "cold USB init failed at %s: %pe\n", stage,
		ERR_PTR(ret));
	unwind = ums9117_musb_cold_shutdown(musb);
	if (unwind)
		dev_err(musb->controller, "cold USB unwind failed: %pe\n",
			ERR_PTR(unwind));
	return ret;
}

static irqreturn_t ums9117_musb_irq(int irq, void *data)
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
	legacy_dma = musb_readl(musb->mregs, UMS9117_MUSB_DMA_MASK_STATUS);
	if (legacy_dma & UMS9117_MUSB_DMA_CHANNEL5_STATUS)
		dma5 = ums9117_musb_quiesce_dma_channel(musb->mregs, 5, 1024);
	if (legacy_dma & UMS9117_MUSB_DMA_CHANNEL21_STATUS)
		dma21 = ums9117_musb_quiesce_dma_channel(musb->mregs, 21, 1024);
	remaining_dma = musb_readl(musb->mregs, UMS9117_MUSB_DMA_MASK_STATUS);
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
	struct ums9117_musb_glue *glue =
		dev_get_drvdata(musb->controller->parent);

	/*
	 * The block exposes flat endpoint registers at 0x100 + ep * 0x10,
	 * while dynamic FIFO sizing is selected through INDEX.  Therefore no
	 * MUSB_INDEXED_EP quirk is used.
	 */
	musb->dyn_fifo = true;
	musb->isr = ums9117_musb_irq;
	if (glue->cold_owned)
		return ums9117_musb_cold_start(musb);
	return ums9117_musb_quiesce_dma(musb, "refusing MUSB handoff");
}

static int ums9117_musb_exit(struct musb *musb)
{
	struct ums9117_musb_glue *glue =
		dev_get_drvdata(musb->controller->parent);
	int ret;

	if (!glue->cold_owned)
		return 0;

	ret = ums9117_musb_cold_shutdown(musb);
	if (ret)
		dev_err(musb->controller, "cold USB shutdown incomplete: %pe\n",
			ERR_PTR(ret));
	return ret;
}

static const struct musb_platform_ops ums9117_musb_ops = {
	.init = ums9117_musb_init,
	.exit = ums9117_musb_exit,
	.read_fifo = ums9117_musb_read_fifo32,
};

static const struct musb_fifo_cfg ums9117_musb_fifo_cfg[] = {
	MUSB_EP_FIFO_SINGLE(1, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(1, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_RX, 512),
};

static const struct musb_hdrc_config ums9117_musb_config = {
	.fifo_cfg = ums9117_musb_fifo_cfg,
	.fifo_cfg_size = ARRAY_SIZE(ums9117_musb_fifo_cfg),
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

static void ums9117_musb_release_vddusb(void *data)
{
	struct ums9117_musb_glue *glue = data;
	int ret;

	if (!glue->vddusb_enabled && !glue->vddusb_force_off)
		return;
	ret = ums9117_musb_enable_analog_gate(glue);
	if (ret) {
		dev_err(glue->dev,
			"could not gate analog registers before vddusb release: %pe\n",
			ERR_PTR(ret));
		return;
	}
	ret = ums9117_musb_isolate_phy(glue);
	if (ret) {
		dev_err(glue->dev,
			"could not isolate PHY before vddusb release: %pe\n",
			ERR_PTR(ret));
		return;
	}
	ret = ums9117_musb_disable_vddusb(glue);
	if (ret) {
		dev_err(glue->dev,
			"cold USB shutdown failed at vddusb devres release: %pe\n",
			ERR_PTR(ret));
		return;
	}
}

static int ums9117_musb_get_cold_resources(struct platform_device *pdev,
					   struct ums9117_musb_glue *glue)
{
	int ret;

	glue->ap_ahb = ums9117_musb_ioremap_shared(pdev, "ap-ahb",
						   UMS9117_MUSB_AP_AHB_BYTES);
	if (IS_ERR(glue->ap_ahb))
		return dev_err_probe(&pdev->dev, PTR_ERR(glue->ap_ahb),
				     "could not map AP AHB registers\n");
	glue->anlg_phy_g8 = ums9117_musb_ioremap_shared(
		pdev, "anlg-phy-g8", UMS9117_MUSB_ANLG_PHY_G8_BYTES);
	if (IS_ERR(glue->anlg_phy_g8))
		return dev_err_probe(&pdev->dev, PTR_ERR(glue->anlg_phy_g8),
				     "could not map G8 USB PHY registers\n");
	glue->anlg_phy_top = ums9117_musb_ioremap_shared(
		pdev, "anlg-phy-top", UMS9117_MUSB_ANLG_PHY_TOP_BYTES);
	if (IS_ERR(glue->anlg_phy_top))
		return dev_err_probe(&pdev->dev, PTR_ERR(glue->anlg_phy_top),
				     "could not map top USB PHY registers\n");
	glue->aon_apb = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							"sprd,aon-apb");
	if (IS_ERR(glue->aon_apb))
		return dev_err_probe(&pdev->dev, PTR_ERR(glue->aon_apb),
				     "could not get AON APB syscon\n");
	glue->vddusb = devm_regulator_get_exclusive(&pdev->dev, "vddusb");
	if (IS_ERR(glue->vddusb))
		return dev_err_probe(&pdev->dev, PTR_ERR(glue->vddusb),
				     "could not get exclusive vddusb supply\n");
	ret = regulator_is_enabled(glue->vddusb);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "could not read vddusb state\n");
	glue->vddusb_enabled = ret;
	ret = devm_add_action_or_reset(&pdev->dev, ums9117_musb_release_vddusb,
				       glue);
	if (ret)
		return ret;
	return 0;
}

static const struct ums9117_musb_match_data ums9117_musb_inherited_data;

static const struct ums9117_musb_match_data ums9117_musb_cold_data = {
	.cold_owned = true,
};

static int ums9117_musb_probe(struct platform_device *pdev)
{
	const struct ums9117_musb_match_data *match_data;
	struct ums9117_musb_glue *glue;
	struct resource *memory;
	int ret;
	int irq;
	struct platform_device_info info = {
		.name = "musb-hdrc",
		.id = PLATFORM_DEVID_AUTO,
		.parent = &pdev->dev,
		.data = &ums9117_musb_pdata,
		.size_data = sizeof(ums9117_musb_pdata),
		.dma_mask = DMA_BIT_MASK(32),
	};

	match_data = of_device_get_match_data(&pdev->dev);
	if (!match_data)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing MUSB match data\n");
	memory = platform_get_resource_byname(pdev, IORESOURCE_MEM, "musb");
	if (!memory)
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
	glue->dev = &pdev->dev;
	glue->cold_owned = match_data->cold_owned;
	if (glue->cold_owned) {
		ret = ums9117_musb_get_cold_resources(pdev, glue);
		if (ret)
			return ret;
	}
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

	return 0;
}

static void ums9117_musb_remove(struct platform_device *pdev)
{
	struct ums9117_musb_glue *glue = platform_get_drvdata(pdev);

	platform_device_unregister(glue->musb);
}

static const struct of_device_id ums9117_musb_of_match[] = {
	{
		.compatible = "fplinux,ums9117-musb-inherited",
		.data = &ums9117_musb_inherited_data,
	},
	{
		.compatible = "fplinux,ums9117-musb",
		.data = &ums9117_musb_cold_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_musb_of_match);

static struct platform_driver ums9117_musb_driver = {
	.probe = ums9117_musb_probe,
	.remove = ums9117_musb_remove,
	.driver = {
		.name = "ums9117-musb",
		.of_match_table = ums9117_musb_of_match,
	},
};
module_platform_driver(ums9117_musb_driver);

MODULE_DESCRIPTION("UMS9117 MUSB gadget glue");
MODULE_LICENSE("GPL");
