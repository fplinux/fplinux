// SPDX-License-Identifier: GPL-2.0-only
/* UMS9117 SPI1 3-wire/9-bit panel transport. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>

#include "ums9117-fb-internal.h"

#define AON_SPI1_GATE_SET 0x1134
#define SPI1_GATE BIT(9)
#define SPI1_RESET BIT(6)
#define SPI_TXD 0x000
#define SPI_CLKD 0x004
#define SPI_CTL0 0x008
#define SPI_CTL1 0x00c
#define SPI_CTL2 0x010
#define SPI_CTL4 0x018
#define SPI_CTL5 0x01c
#define SPI_INT_EN 0x020
#define SPI_INT_CLR 0x024
#define SPI_INT_RAW 0x028
#define SPI_STS2 0x034
#define SPI_CTL7 0x04c
#define SPI_CTL8 0x054
#define SPI_CTL9 0x058
#define SPI_CTL12 0x064
#define SPI_TX_END BIT(8)
#define SPI_CS0 BIT(8)
#define SPI_MODE_3WIRE_9BIT BIT(3)
#define SPI_TX_HOLD BIT(7)
#define SPI_RGB565 BIT(14)
#define SPI_PIXEL_BITS 17
#define SPI_INIT_DIVIDER 3
#define SPI_DCS_TIMEOUT_US 250000

static int ums9117_spi_wait_idle(void __iomem *spi, ktime_t deadline)
{
	unsigned int remaining;
	s64 delta;
	u32 value;

	delta = ktime_us_delta(deadline, ktime_get());
	if (delta <= 0)
		return -ETIMEDOUT;
	remaining = delta;
	if (readl_poll_timeout_atomic(spi + SPI_STS2, value, value & BIT(7), 1,
				      remaining))
		return -ETIMEDOUT;
	delta = ktime_us_delta(deadline, ktime_get());
	if (delta <= 0)
		return -ETIMEDOUT;
	if (readl_poll_timeout_atomic(spi + SPI_STS2, value, !(value & BIT(8)),
				      1, delta))
		return -ETIMEDOUT;
	return 0;
}

static void ums9117_spi_channel_length(struct ums9117_fb *ufb, u32 bits)
{
	u32 value = readl(ufb->spi + SPI_CTL0);

	value &= ~(0x1f << 2);
	value |= (bits & 0x1f) << 2;
	writel(value, ufb->spi + SPI_CTL0);
}

static void ums9117_spi_tx_length(struct ums9117_fb *ufb, u32 words)
{
	u32 ctl8 = readl(ufb->spi + SPI_CTL8) & ~0x3ff;
	u32 ctl9 = readl(ufb->spi + SPI_CTL9) & ~0xffff;

	ctl8 |= words >> 16;
	ctl9 |= words & 0xffff;
	writel(ctl8, ufb->spi + SPI_CTL8);
	writel(ctl9, ufb->spi + SPI_CTL9);
}

static void ums9117_spi_pixel_mode(struct ums9117_fb *ufb)
{
	writel(readl(ufb->spi + SPI_CTL8) | BIT(15), ufb->spi + SPI_CTL8);
	writel(readl(ufb->spi + SPI_CTL7) | SPI_RGB565, ufb->spi + SPI_CTL7);
	ums9117_spi_channel_length(ufb, SPI_PIXEL_BITS);
}

static int ums9117_spi_word(struct ums9117_fb *ufb, u8 value, bool command,
			    bool restore_pixel)
{
	ktime_t deadline = ktime_add_us(ktime_get(), SPI_DCS_TIMEOUT_US);
	s64 delta;
	u32 status;
	int ret;

	ret = ums9117_spi_wait_idle(ufb->spi, deadline);
	if (ret)
		goto out;
	writel(SPI_TX_END, ufb->spi + SPI_INT_CLR);
	writel(readl(ufb->spi + SPI_CTL7) & ~SPI_RGB565, ufb->spi + SPI_CTL7);
	ums9117_spi_channel_length(ufb, 8);
	if (command)
		writel(readl(ufb->spi + SPI_CTL8) & ~BIT(15),
		       ufb->spi + SPI_CTL8);
	else
		writel(readl(ufb->spi + SPI_CTL8) | BIT(15),
		       ufb->spi + SPI_CTL8);
	ums9117_spi_tx_length(ufb, 1);
	writel(readl(ufb->spi + SPI_CTL12) | BIT(1), ufb->spi + SPI_CTL12);
	writel(value, ufb->spi + SPI_TXD);
	delta = ktime_us_delta(deadline, ktime_get());
	if (delta <= 0 ||
	    readl_poll_timeout_atomic(ufb->spi + SPI_INT_RAW, status,
				      status & SPI_TX_END, 1, delta)) {
		ret = -ETIMEDOUT;
		goto out;
	}
	writel(SPI_TX_END, ufb->spi + SPI_INT_CLR);
	ret = ums9117_spi_wait_idle(ufb->spi, deadline);
out:
	if (restore_pixel)
		ums9117_spi_pixel_mode(ufb);
	return ret;
}

static int ums9117_spi_init(struct ums9117_fb *ufb,
			    struct platform_device *pdev)
{
	struct resource *resource;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "spi");
	ufb->spi = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(ufb->spi))
		return PTR_ERR(ufb->spi);
	ufb->stream_phys = resource->start + SPI_TXD;
	ufb->lcm = devm_platform_ioremap_resource_byname(pdev, "lcm");
	if (IS_ERR(ufb->lcm))
		return PTR_ERR(ufb->lcm);
	ufb->spi_clock_selector = devm_platform_ioremap_resource_byname(
		pdev, "spi-clock-selector");
	if (IS_ERR(ufb->spi_clock_selector))
		return PTR_ERR(ufb->spi_clock_selector);
	ufb->spi_reset_set =
		devm_platform_ioremap_resource_byname(pdev, "spi-reset-set");
	if (IS_ERR(ufb->spi_reset_set))
		return PTR_ERR(ufb->spi_reset_set);
	ufb->spi_reset_clear =
		devm_platform_ioremap_resource_byname(pdev, "spi-reset-clear");
	return IS_ERR(ufb->spi_reset_clear) ? PTR_ERR(ufb->spi_reset_clear) : 0;
}

static int ums9117_spi_enable(struct ums9117_fb *ufb)
{
	u32 value;
	int ret;

	ret = regmap_write(ufb->aon_apb, AON_SPI1_GATE_SET, SPI1_GATE);
	if (ret)
		return ret;
	writel(3, ufb->spi_clock_selector);
	writel(0, ufb->lcm + 0x000);
	writel(1, ufb->lcm + 0x010);
	writel(0x00a50100, ufb->lcm + 0x014);
	writel(SPI1_RESET, ufb->spi_reset_set);
	usleep_range(1000, 2000);
	writel(SPI1_RESET, ufb->spi_reset_clear);
	usleep_range(1000, 2000);
	writel(0, ufb->spi + SPI_INT_EN);
	writel(0xf00 | 2 | (8 << 2), ufb->spi + SPI_CTL0);
	value = readl(ufb->spi + SPI_CTL1);
	writel((value & ~0x3000) | 0x3000, ufb->spi + SPI_CTL1);
	writel((readl(ufb->spi + SPI_CTL2) & ~0x1f) | 7, ufb->spi + SPI_CTL2);
	writel(0x8000, ufb->spi + SPI_CTL4);
	writel(0, ufb->spi + SPI_CTL5);
	writel(SPI_INIT_DIVIDER, ufb->spi + SPI_CLKD);
	writel(SPI_MODE_3WIRE_9BIT, ufb->spi + SPI_CTL7);
	writel(0, ufb->spi + SPI_CTL8);
	writel(SPI_TX_END, ufb->spi + SPI_INT_CLR);
	return 0;
}

int ums9117_fb_spi_dcs(struct ums9117_fb *ufb, u8 command, const u8 *data,
		       size_t length)
{
	size_t i;
	int ret;

	ret = ums9117_spi_word(ufb, command, true, false);
	for (i = 0; !ret && i < length; i++)
		ret = ums9117_spi_word(ufb, data[i], false, false);
	ums9117_spi_pixel_mode(ufb);
	return ret;
}

int ums9117_fb_spi_begin_frame(struct ums9117_fb *ufb)
{
	int ret;

	/* Nokia's proven run path is divider 0 with TX_HOLD before every 2c. */
	writel(0, ufb->spi + SPI_CLKD);
	writel(readl(ufb->spi + SPI_CTL7) | SPI_TX_HOLD, ufb->spi + SPI_CTL7);
	ret = ums9117_fb_spi_dcs(ufb, 0x2c, NULL, 0);
	if (!ret) {
		u32 pixels = ufb->profile->width * ufb->profile->height;

		ums9117_spi_tx_length(ufb, pixels);
		writel(readl(ufb->spi + SPI_CTL12) | BIT(1),
		       ufb->spi + SPI_CTL12);
	}
	return ret;
}

int ums9117_fb_spi_init_transport(struct ums9117_fb *ufb,
				  struct platform_device *pdev)
{
	return ums9117_spi_init(ufb, pdev);
}

int ums9117_fb_spi_enable_transport(struct ums9117_fb *ufb)
{
	return ums9117_spi_enable(ufb);
}

int ums9117_fb_spi_post_reset(struct ums9117_fb *ufb)
{
	/* Select the panel chip before the DCS init sequence runs. */
	writel(readl(ufb->spi + SPI_CTL0) & ~SPI_CS0, ufb->spi + SPI_CTL0);
	return 0;
}
