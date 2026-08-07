// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bootstrap-handoff framebuffer for Nokia 3210 4G TA-1618.
 *
 * fpdoom initializes the ST7789P3 panel, SPI1 and LCDC before Linux starts.
 * This driver registers the reserved RGB565 buffer as fb0 and repeatedly
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
#define LCDC_IRQ_RAW 0x11c
#define LCDC_DONE BIT(0)
#define LCDC_RUN BIT(3)
#define LCDC_FMARK_OFF BIT(1)
#define LCDC_FMARK_POL BIT(2)
#define LCDC_RGB_MODE (7u << 5)

#define SPI_TXD 0x000
#define SPI_CLKD 0x004
#define SPI_CTL0 0x008
#define SPI_INT_CLR 0x024
#define SPI_INT_RAW 0x028
#define SPI_STS2 0x034
#define SPI_CTL7 0x04c
#define SPI_CTL8 0x054
#define SPI_CTL9 0x058
#define SPI_CTL12 0x064
#define SPI_MODE (7u << 3)
#define SPI_MODE_3WIRE_9BIT (1u << 3)
#define SPI_LANE2 BIT(15)
#define SPI_LANE2_PIN BIT(13)

#define SPI_DIVIDER 0u
#define PIXEL_BITS 17u

#define PANEL_TE_ON 0x35
#define PANEL_FRAME_RATE 0xc6
#define PANEL_WRITE_RAM 0x2c

/* Stretches the line period so the scan cannot overtake the write. */
#define PANEL_LINE_PERIOD 0x18

#define TRANSFER_TIMEOUT_US 250000

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
	unsigned int shown;
	bool armed;
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

/* Sleeping here would cost a whole 10 ms tick for a wait of microseconds. */
static int spi_wait_idle(void __iomem *spi)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout_atomic(spi + SPI_STS2, v, v & BIT(7), 1,
					TRANSFER_TIMEOUT_US);
	if (ret)
		return ret;
	return readl_poll_timeout_atomic(spi + SPI_STS2, v, !(v & BIT(8)), 1,
					 TRANSFER_TIMEOUT_US);
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

	ret = readl_poll_timeout_atomic(spi + SPI_INT_RAW, v, v & BIT(8), 1,
					TRANSFER_TIMEOUT_US);
	if (ret)
		return ret;
	writel(BIT(8), spi + SPI_INT_CLR);
	ret = spi_wait_idle(spi);
	if (ret)
		return ret;

	writel(readl(spi + SPI_CTL8) | BIT(15), spi + SPI_CTL8);
	writel(readl(spi + SPI_CTL7) | BIT(14), spi + SPI_CTL7);
	spi_channel_length(spi, PIXEL_BITS);
	return 0;
}

/* The command byte carries a clear data bit; a parameter carries a set one. */
static int spi_send_param(void __iomem *spi, u8 value)
{
	u32 v;
	int ret;

	ret = spi_wait_idle(spi);
	if (ret)
		return ret;
	writel(readl(spi + SPI_CTL7) & ~BIT(14), spi + SPI_CTL7);
	spi_channel_length(spi, 8);
	spi_tx_length(spi, 1);
	writel(readl(spi + SPI_CTL12) | BIT(1), spi + SPI_CTL12);
	writel(value, spi + SPI_TXD);
	ret = readl_poll_timeout_atomic(spi + SPI_INT_RAW, v, v & BIT(8), 1,
					TRANSFER_TIMEOUT_US);
	if (ret)
		return ret;
	writel(BIT(8), spi + SPI_INT_CLR);
	writel(readl(spi + SPI_CTL7) | BIT(14), spi + SPI_CTL7);
	spi_channel_length(spi, PIXEL_BITS);
	return 0;
}

static int ta1618_send_panel(void __iomem *spi, u8 command, u8 param)
{
	int ret = spi_send_command(spi, command);

	if (ret)
		return ret;
	return spi_send_param(spi, param);
}

static int ta1618_refresh(struct ta1618_fb *tfb)
{
	u32 done;
	u32 v;
	int ret;

	/*
	 * The link reads idle while a frame is armed and waiting on the panel,
	 * so the controller's own done bit is the only honest end of one.  There
	 * is nothing to wait for until a frame has been armed, and a wait that
	 * expires leaves nothing armed either.
	 */
	if (tfb->armed) {
		ret = readl_poll_timeout_atomic(tfb->lcdc + LCDC_IRQ_RAW, done,
						done & LCDC_DONE, 10,
						TRANSFER_TIMEOUT_US);
		tfb->armed = false;
		if (ret)
			return ret;
		writel(LCDC_DONE, tfb->lcdc + LCDC_IRQ_CLR);
	}

	ret = spi_send_command(tfb->spi, PANEL_WRITE_RAM);
	if (ret)
		return ret;

	/* Staged so a drawing program cannot change what the controller fetches. */
	memcpy_fromio(tfb->snapshot, tfb->screen + tfb->shown * FB_STRIDE,
		      FB_SIZE);
	memcpy_toio(tfb->transfer, tfb->snapshot, FB_SIZE);
	/* Drain write combining: the controller fetches from DRAM on its own. */
	wmb();

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

	/* Latch the layer and capture setup before the start bit below. */
	wmb();
	writel(LCDC_DONE, tfb->lcdc + LCDC_IRQ_CLR);
	writel(readl(tfb->lcdc + LCDC_IRQ_EN) | LCDC_DONE,
	       tfb->lcdc + LCDC_IRQ_EN);

	v = readl(tfb->lcdc + LCDC_CTRL);
	v &= ~LCDC_RGB_MODE;
	/* Clear waits for the panel; the polarity picks the edge inside blanking. */
	v &= ~LCDC_FMARK_OFF;
	v |= LCDC_FMARK_POL;
	writel(v | LCDC_RUN, tfb->lcdc + LCDC_CTRL);
	tfb->armed = true;
	return 0;
}

static void ta1618_refresh_work(struct work_struct *work)
{
	struct ta1618_fb *tfb = container_of(to_delayed_work(work),
					     struct ta1618_fb, refresh_work);

	if (ta1618_refresh(tfb))
		dev_warn_ratelimited(tfb->info->device,
				     "display refresh timed out\n");
	schedule_delayed_work(&tfb->refresh_work, 0);
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

/* Twice the height is optional: a program unaware of it still runs. */
static int ta1618_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if (var->xres != FB_WIDTH || var->yres != FB_HEIGHT ||
	    var->bits_per_pixel != 16)
		return -EINVAL;
	if (var->xres_virtual != FB_WIDTH)
		return -EINVAL;
	if (var->yres_virtual != FB_HEIGHT &&
	    var->yres_virtual != 2 * FB_HEIGHT)
		return -EINVAL;
	if (var->yoffset + FB_HEIGHT > var->yres_virtual)
		return -EINVAL;
	return 0;
}

static int ta1618_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info)
{
	struct ta1618_fb *tfb = info->par;

	if (var->yoffset != 0 && var->yoffset != FB_HEIGHT)
		return -EINVAL;
	tfb->shown = var->yoffset;
	return 0;
}

static const struct fb_ops ta1618_fb_ops = {
	.owner = THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	.fb_setcolreg = ta1618_setcolreg,
	.fb_check_var = ta1618_check_var,
	.fb_pan_display = ta1618_pan_display,
};

static void ta1618_start_panel(struct ta1618_fb *tfb)
{
	u32 ctl7 = readl(tfb->spi + SPI_CTL7);

	writel(SPI_DIVIDER, tfb->spi + SPI_CLKD);
	/* The link keeps its shifting mode across a module reload. */
	ctl7 = (ctl7 & ~(SPI_MODE | SPI_LANE2)) | SPI_MODE_3WIRE_9BIT;
	writel(ctl7, tfb->spi + SPI_CTL7);
	writel(readl(tfb->spi + SPI_CTL8) & ~SPI_LANE2_PIN,
	       tfb->spi + SPI_CTL8);

	ta1618_send_panel(tfb->spi, PANEL_TE_ON, 0x00);
	ta1618_send_panel(tfb->spi, PANEL_FRAME_RATE, PANEL_LINE_PERIOD);
}

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
	if (resource_size(fbres) < 3 * FB_SIZE) {
		ret = -EINVAL;
		goto release;
	}
	tfb->screen_phys = fbres->start;
	tfb->screen = devm_ioremap_wc(dev, fbres->start, resource_size(fbres));
	if (!tfb->screen) {
		ret = -ENOMEM;
		goto release;
	}
	tfb->transfer_phys = tfb->screen_phys + 2 * FB_SIZE;
	tfb->transfer = tfb->screen + 2 * FB_SIZE;
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
	info->fix.ypanstep = 1;
	info->fix.smem_start = tfb->screen_phys;
	info->fix.smem_len = 2 * FB_SIZE;
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
	info->screen_size = 2 * FB_SIZE;
	info->pseudo_palette = tfb->pseudo_palette;

	ret = fb_alloc_cmap(&info->cmap, 16, 0);
	if (ret)
		goto release;
	ret = register_framebuffer(info);
	if (ret)
		goto cmap;

	platform_set_drvdata(pdev, tfb);
	ta1618_start_panel(tfb);
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
	/*
	 * Every frame arms the next one before it returns, so the loop always
	 * stops with a frame waiting on the panel.  Left that way the controller
	 * stays a second owner of the link after this driver is gone.
	 */
	if (tfb->armed) {
		u32 done;

		readl_poll_timeout(tfb->lcdc + LCDC_IRQ_RAW, done,
				   done & LCDC_DONE, 100, TRANSFER_TIMEOUT_US);
		writel(LCDC_DONE, tfb->lcdc + LCDC_IRQ_CLR);
	}
	writel(readl(tfb->lcdc + LCDC_CTRL) & ~(BIT(0) | LCDC_RUN),
	       tfb->lcdc + LCDC_CTRL);
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
