// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bootstrap-handoff framebuffer for Nokia 3210 4G TA-1618.
 *
 * fpdoom initializes the ST7789P3 panel, SPI1 and LCDC before Linux starts.
 * This driver registers the reserved RGB565 buffer as fb0 and periodically
 * triggers the same LCDC -> SPI1 transfer sequence.  The polling transfer path
 * does not require a display IRQ.
 */
#include <linux/bitops.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define FB_WIDTH 240u
#define FB_HEIGHT 320u
#define FB_STRIDE (FB_WIDTH * 2u)
#define FB_SIZE (FB_STRIDE * FB_HEIGHT)

#define LCDC_CTRL 0x000
#define LCDC_DISP_SIZE 0x004
#define LCDC_LCM_START 0x008
#define LCDC_LCM_SIZE 0x00c
#define LCDC_BG_COLOR 0x010
#define LCDC_IMG_CTRL 0x020
#define LCDC_IMG_Y_BASE 0x024
#define LCDC_IMG_SIZE_XY 0x02c
#define LCDC_IMG_PITCH 0x030
#define LCDC_IMG_DISP_XY 0x034
#define LCDC_CAP_CTRL 0x0e0
#define LCDC_CAP_BASE 0x0e4
#define LCDC_IRQ_EN 0x110
#define LCDC_IRQ_CLR 0x114

#define SPI_TXD 0x000
#define SPI_CTL0 0x008
#define SPI_INT_CLR 0x024
#define SPI_INT_RAW 0x028
#define SPI_STS2 0x034
#define SPI_CTL7 0x04c
#define SPI_CTL8 0x054
#define SPI_CTL9 0x058
#define SPI_CTL12 0x064

struct ta1618_fb {
	struct fb_info *info;
	void __iomem *screen;
	void __iomem *transfer;
	void *snapshot;
	void __iomem *lcdc;
	void __iomem *spi;
	phys_addr_t screen_phys;
	phys_addr_t transfer_phys;
	struct delayed_work refresh_work;
	u32 pseudo_palette[16];
};

static void spi_channel_length(void __iomem *spi, unsigned int bits)
{
	u32 v = readl(spi + SPI_CTL0);

	v &= ~(0x1f << 2);
	v |= (bits & 0x1f) << 2;
	writel(v, spi + SPI_CTL0);
}

static void spi_tx_length(void __iomem *spi, unsigned int words)
{
	u32 ctl8 = readl(spi + SPI_CTL8) & ~0x3ff;
	u32 ctl9 = readl(spi + SPI_CTL9) & ~0xffff;

	ctl8 |= words >> 16;
	ctl9 |= words & 0xffff;
	writel(ctl8, spi + SPI_CTL8);
	writel(ctl9, spi + SPI_CTL9);
}

static int spi_wait_idle(void __iomem *spi)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(spi + SPI_STS2, v, v & BIT(7), 10, 250000);
	if (ret)
		return ret;
	return readl_poll_timeout(spi + SPI_STS2, v, !(v & BIT(8)), 10, 250000);
}

static int spi_send_command(void __iomem *spi, u8 command)
{
	u32 v;
	int ret;

	ret = spi_wait_idle(spi);
	if (ret)
		return ret;

	writel(readl(spi + SPI_CTL7) & ~BIT(14), spi + SPI_CTL7);
	spi_channel_length(spi, 8);
	writel(readl(spi + SPI_CTL8) & ~BIT(15), spi + SPI_CTL8);
	spi_tx_length(spi, 1);
	writel(readl(spi + SPI_CTL12) | BIT(1), spi + SPI_CTL12);
	writel(command, spi + SPI_TXD);

	ret = readl_poll_timeout(spi + SPI_INT_RAW, v, v & BIT(8), 10, 250000);
	if (ret)
		return ret;
	writel(BIT(8), spi + SPI_INT_CLR);
	ret = spi_wait_idle(spi);
	if (ret)
		return ret;

	writel(readl(spi + SPI_CTL8) | BIT(15), spi + SPI_CTL8);
	writel(readl(spi + SPI_CTL7) | BIT(14), spi + SPI_CTL7);
	spi_channel_length(spi, 17);
	return 0;
}

static int ta1618_refresh(struct ta1618_fb *tfb)
{
	u32 v;
	int ret;

	ret = spi_send_command(tfb->spi, 0x2c);
	if (ret)
		return ret;

	/*
	 * fbcon updates screen while the panel transfer is in flight. DMA from
	 * that live buffer produces visibly split/overwritten rows. SPI is idle
	 * here, so copy the current frame into the second half of reserved memory
	 * and let LCDC read only that buffer. Unsynchronized fbdev writers can
	 * still change screen during the copy, but cannot change the DMA source.
	 */
	memcpy_fromio(tfb->snapshot, tfb->screen, FB_SIZE);
	memcpy_toio(tfb->transfer, tfb->snapshot, FB_SIZE);
	/*
	 * The staging copy went through a write-combining mapping. Drain
	 * the CPU write-combining buffers before the transfer is armed, so
	 * LCDC, an independent bus master fetching from DRAM, reads a
	 * fully written frame instead of torn rows.
	 */
	wmb();

	/* One RGB565 word per pixel, software-triggered SPI transfer. */
	spi_tx_length(tfb->spi, FB_WIDTH * FB_HEIGHT);
	writel(readl(tfb->spi + SPI_CTL12) | BIT(1), tfb->spi + SPI_CTL12);

	/* Reassert the handoff state in case fbcon initialized after bootstrap. */
	writel(readl(tfb->lcdc + LCDC_CTRL) | BIT(0), tfb->lcdc + LCDC_CTRL);
	writel(FB_WIDTH | (FB_HEIGHT << 16), tfb->lcdc + LCDC_DISP_SIZE);
	writel(0, tfb->lcdc + LCDC_LCM_START);
	writel(FB_WIDTH | (FB_HEIGHT << 16), tfb->lcdc + LCDC_LCM_SIZE);
	writel(0, tfb->lcdc + LCDC_BG_COLOR);

	v = readl(tfb->lcdc + LCDC_IMG_CTRL);
	v &= ~BIT(1);
	v = (v & ~(0xf << 4)) | (5 << 4);
	v = (v & ~(3 << 8)) | (2 << 8);
	v |= BIT(0);
	writel(v, tfb->lcdc + LCDC_IMG_CTRL);
	writel((u32)(tfb->transfer_phys >> 2), tfb->lcdc + LCDC_IMG_Y_BASE);
	writel(FB_WIDTH | (FB_HEIGHT << 16), tfb->lcdc + LCDC_IMG_SIZE_XY);
	writel(FB_WIDTH, tfb->lcdc + LCDC_IMG_PITCH);
	writel(0, tfb->lcdc + LCDC_IMG_DISP_XY);

	v = readl(tfb->lcdc + LCDC_CAP_CTRL);
	v &= ~(3 << 6);
	v |= 0x20;
	writel(v, tfb->lcdc + LCDC_CAP_CTRL);
	writel((0x70b00000u + SPI_TXD) >> 2, tfb->lcdc + LCDC_CAP_BASE);

	/*
	 * Configuration/start boundary of the vendor refresh sequence: the
	 * image-layer and capture programming above must be latched before
	 * the interrupt ack and the frame-start bit below. Defensive
	 * ordering point: writel() is already ordered, and no reordering
	 * has been observed at this boundary.
	 */
	wmb();
	writel(BIT(0), tfb->lcdc + LCDC_IRQ_CLR);
	writel(readl(tfb->lcdc + LCDC_IRQ_EN) | BIT(0),
	       tfb->lcdc + LCDC_IRQ_EN);
	writel(readl(tfb->lcdc + LCDC_CTRL) | BIT(3), tfb->lcdc + LCDC_CTRL);
	return 0;
}

static void ta1618_refresh_work(struct work_struct *work)
{
	struct ta1618_fb *tfb = container_of(to_delayed_work(work),
					     struct ta1618_fb, refresh_work);

	if (ta1618_refresh(tfb))
		dev_warn_ratelimited(tfb->info->device,
				     "display refresh timed out\n");
	schedule_delayed_work(&tfb->refresh_work, msecs_to_jiffies(100));
}

static int ta1618_setcolreg(unsigned int regno, unsigned int red,
			    unsigned int green, unsigned int blue,
			    unsigned int transp, struct fb_info *info)
{
	struct ta1618_fb *tfb = info->par;

	if (regno >= ARRAY_SIZE(tfb->pseudo_palette))
		return -EINVAL;
	tfb->pseudo_palette[regno] = ((red >> 11) << 11) |
				     ((green >> 10) << 5) | (blue >> 11);
	return 0;
}

static const struct fb_ops ta1618_fb_ops = {
	.owner = THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	.fb_setcolreg = ta1618_setcolreg,
};

static int ta1618_fb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *fbres;
	struct fb_info *info;
	struct ta1618_fb *tfb;
	int ret;

	info = framebuffer_alloc(sizeof(*tfb), dev);
	if (!info)
		return -ENOMEM;
	tfb = info->par;
	tfb->info = info;
	tfb->snapshot = kvmalloc(FB_SIZE, GFP_KERNEL);
	if (!tfb->snapshot) {
		ret = -ENOMEM;
		goto release;
	}

	fbres = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!fbres) {
		ret = -ENODEV;
		goto release;
	}
	if (resource_size(fbres) < 2 * FB_SIZE) {
		ret = -EINVAL;
		goto release;
	}
	tfb->screen_phys = fbres->start;
	tfb->screen = devm_ioremap_wc(dev, fbres->start, resource_size(fbres));
	if (!tfb->screen) {
		ret = -ENOMEM;
		goto release;
	}
	tfb->transfer_phys = tfb->screen_phys + FB_SIZE;
	tfb->transfer = tfb->screen + FB_SIZE;
	tfb->lcdc = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(tfb->lcdc)) {
		ret = PTR_ERR(tfb->lcdc);
		goto release;
	}
	tfb->spi = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(tfb->spi)) {
		ret = PTR_ERR(tfb->spi);
		goto release;
	}

	strscpy(info->fix.id, "ta1618-rgb565", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.smem_start = tfb->screen_phys;
	info->fix.smem_len = FB_SIZE;
	info->fix.line_length = FB_STRIDE;

	info->var.xres = FB_WIDTH;
	info->var.yres = FB_HEIGHT;
	info->var.xres_virtual = FB_WIDTH;
	info->var.yres_virtual = FB_HEIGHT;
	info->var.bits_per_pixel = 16;
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.vmode = FB_VMODE_NONINTERLACED;
	info->fbops = &ta1618_fb_ops;
	info->screen_base = tfb->screen;
	info->screen_size = FB_SIZE;
	info->pseudo_palette = tfb->pseudo_palette;

	ret = fb_alloc_cmap(&info->cmap, 16, 0);
	if (ret)
		goto release;
	ret = register_framebuffer(info);
	if (ret)
		goto cmap;

	platform_set_drvdata(pdev, tfb);
	INIT_DELAYED_WORK(&tfb->refresh_work, ta1618_refresh_work);
	schedule_delayed_work(&tfb->refresh_work, 0);
	dev_info(dev, "TA-1618 RGB565 framebuffer registered as fb%d\n",
		 info->node);
	return 0;

cmap:
	fb_dealloc_cmap(&info->cmap);
release:
	kvfree(tfb->snapshot);
	framebuffer_release(info);
	return ret;
}

static void ta1618_fb_remove(struct platform_device *pdev)
{
	struct ta1618_fb *tfb = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&tfb->refresh_work);
	unregister_framebuffer(tfb->info);
	fb_dealloc_cmap(&tfb->info->cmap);
	kvfree(tfb->snapshot);
	framebuffer_release(tfb->info);
}

static const struct of_device_id ta1618_fb_of_match[] = {
	{ .compatible = "fplinux,ta1618-fb" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_fb_of_match);

static struct platform_driver ta1618_fb_driver = {
	.probe = ta1618_fb_probe,
	.remove = ta1618_fb_remove,
	.driver = {
		.name = "ta1618-fb",
		.of_match_table = ta1618_fb_of_match,
	},
};
module_platform_driver(ta1618_fb_driver);

MODULE_DESCRIPTION("Nokia TA-1618 bootstrap framebuffer");
MODULE_LICENSE("GPL");
