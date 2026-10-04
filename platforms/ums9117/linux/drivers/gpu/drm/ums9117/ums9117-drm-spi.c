// SPDX-License-Identifier: GPL-2.0-only
/* UMS9117 SPI1 3-wire/9-bit panel transport. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <video/mipi_display.h>

#include "ums9117-drm-internal.h"

#define UMS9117_AON_SPI1_GATE_SET 0x1134
#define UMS9117_AON_SPI1_GATE BIT(9)
#define UMS9117_AP_AHB_SPI1_RESET BIT(6)
#define UMS9117_DRM_SPI_TXD 0x000
#define UMS9117_DRM_SPI_CLKD 0x004
#define UMS9117_DRM_SPI_CTL0 0x008
#define UMS9117_DRM_SPI_CTL1 0x00c
#define UMS9117_DRM_SPI_CTL2 0x010
#define UMS9117_DRM_SPI_CTL4 0x018
#define UMS9117_DRM_SPI_CTL5 0x01c
#define UMS9117_DRM_SPI_INT_EN 0x020
#define UMS9117_DRM_SPI_INT_CLR 0x024
#define UMS9117_DRM_SPI_INT_RAW 0x028
#define UMS9117_DRM_SPI_STS2 0x034
#define UMS9117_DRM_SPI_CTL7 0x04c
#define UMS9117_DRM_SPI_CTL8 0x054
#define UMS9117_DRM_SPI_CTL9 0x058
#define UMS9117_DRM_SPI_CTL12 0x064
#define UMS9117_DRM_SPI_TX_END BIT(8)
#define UMS9117_DRM_SPI_CS0 BIT(8)
#define UMS9117_DRM_SPI_MODE_3WIRE_9BIT BIT(3)
#define UMS9117_DRM_SPI_TX_HOLD BIT(7)
#define UMS9117_DRM_SPI_RGB565 BIT(14)
#define UMS9117_DRM_SPI_PIXEL_BITS 17
#define UMS9117_DRM_SPI_INIT_DIVIDER 3
#define UMS9117_DRM_SPI_DCS_TIMEOUT_US 250000

static int ums9117_drm_spi_wait_idle(void __iomem *spi, ktime_t deadline)
{
	unsigned int remaining;
	s64 delta;
	u32 value;

	delta = ktime_us_delta(deadline, ktime_get());
	if (delta <= 0)
		return -ETIMEDOUT;
	remaining = delta;
	if (readl_poll_timeout_atomic(spi + UMS9117_DRM_SPI_STS2, value,
				      value & BIT(7), 1, remaining))
		return -ETIMEDOUT;
	delta = ktime_us_delta(deadline, ktime_get());
	if (delta <= 0)
		return -ETIMEDOUT;
	if (readl_poll_timeout_atomic(spi + UMS9117_DRM_SPI_STS2, value,
				      !(value & BIT(8)), 1, delta))
		return -ETIMEDOUT;
	return 0;
}

static void ums9117_drm_spi_channel_length(struct ums9117_drm *udrm, u32 bits)
{
	u32 value = readl(udrm->spi + UMS9117_DRM_SPI_CTL0);

	value &= ~(0x1f << 2);
	value |= (bits & 0x1f) << 2;
	writel(value, udrm->spi + UMS9117_DRM_SPI_CTL0);
}

static void ums9117_drm_spi_tx_length(struct ums9117_drm *udrm, u32 words)
{
	u32 ctl8 = readl(udrm->spi + UMS9117_DRM_SPI_CTL8) & ~0x3ff;
	u32 ctl9 = readl(udrm->spi + UMS9117_DRM_SPI_CTL9) & ~0xffff;

	ctl8 |= words >> 16;
	ctl9 |= words & 0xffff;
	writel(ctl8, udrm->spi + UMS9117_DRM_SPI_CTL8);
	writel(ctl9, udrm->spi + UMS9117_DRM_SPI_CTL9);
}

static void ums9117_drm_spi_pixel_mode(struct ums9117_drm *udrm)
{
	writel(readl(udrm->spi + UMS9117_DRM_SPI_CTL8) | BIT(15),
	       udrm->spi + UMS9117_DRM_SPI_CTL8);
	writel(readl(udrm->spi + UMS9117_DRM_SPI_CTL7) | UMS9117_DRM_SPI_RGB565,
	       udrm->spi + UMS9117_DRM_SPI_CTL7);
	ums9117_drm_spi_channel_length(udrm, UMS9117_DRM_SPI_PIXEL_BITS);
}

static int ums9117_drm_spi_word(struct ums9117_drm *udrm, u8 value,
				bool command, bool restore_pixel)
{
	ktime_t deadline =
		ktime_add_us(ktime_get(), UMS9117_DRM_SPI_DCS_TIMEOUT_US);
	s64 delta;
	u32 status;
	int ret;

	ret = ums9117_drm_spi_wait_idle(udrm->spi, deadline);
	if (ret)
		goto out;
	writel(UMS9117_DRM_SPI_TX_END, udrm->spi + UMS9117_DRM_SPI_INT_CLR);
	writel(readl(udrm->spi + UMS9117_DRM_SPI_CTL7) &
		       ~UMS9117_DRM_SPI_RGB565,
	       udrm->spi + UMS9117_DRM_SPI_CTL7);
	ums9117_drm_spi_channel_length(udrm, 8);
	if (command)
		writel(readl(udrm->spi + UMS9117_DRM_SPI_CTL8) & ~BIT(15),
		       udrm->spi + UMS9117_DRM_SPI_CTL8);
	else
		writel(readl(udrm->spi + UMS9117_DRM_SPI_CTL8) | BIT(15),
		       udrm->spi + UMS9117_DRM_SPI_CTL8);
	ums9117_drm_spi_tx_length(udrm, 1);
	writel(readl(udrm->spi + UMS9117_DRM_SPI_CTL12) | BIT(1),
	       udrm->spi + UMS9117_DRM_SPI_CTL12);
	writel(value, udrm->spi + UMS9117_DRM_SPI_TXD);
	delta = ktime_us_delta(deadline, ktime_get());
	if (delta <= 0 || readl_poll_timeout_atomic(
				  udrm->spi + UMS9117_DRM_SPI_INT_RAW, status,
				  status & UMS9117_DRM_SPI_TX_END, 1, delta)) {
		ret = -ETIMEDOUT;
		goto out;
	}
	writel(UMS9117_DRM_SPI_TX_END, udrm->spi + UMS9117_DRM_SPI_INT_CLR);
	ret = ums9117_drm_spi_wait_idle(udrm->spi, deadline);
out:
	if (restore_pixel)
		ums9117_drm_spi_pixel_mode(udrm);
	return ret;
}

static int ums9117_drm_spi_init(struct ums9117_drm *udrm,
				struct platform_device *pdev)
{
	struct resource *resource;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "spi");
	udrm->spi = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(udrm->spi))
		return PTR_ERR(udrm->spi);
	udrm->stream_phys = resource->start + UMS9117_DRM_SPI_TXD;
	udrm->lcm = devm_platform_ioremap_resource_byname(pdev, "lcm");
	if (IS_ERR(udrm->lcm))
		return PTR_ERR(udrm->lcm);
	udrm->spi_clock_selector = devm_platform_ioremap_resource_byname(
		pdev, "spi-clock-selector");
	if (IS_ERR(udrm->spi_clock_selector))
		return PTR_ERR(udrm->spi_clock_selector);
	udrm->spi_reset_set =
		devm_platform_ioremap_resource_byname(pdev, "spi-reset-set");
	if (IS_ERR(udrm->spi_reset_set))
		return PTR_ERR(udrm->spi_reset_set);
	udrm->spi_reset_clear =
		devm_platform_ioremap_resource_byname(pdev, "spi-reset-clear");
	return IS_ERR(udrm->spi_reset_clear) ? PTR_ERR(udrm->spi_reset_clear) :
					       0;
}

static int ums9117_drm_spi_enable(struct ums9117_drm *udrm)
{
	u32 value;
	int ret;

	ret = regmap_write(udrm->aon_apb, UMS9117_AON_SPI1_GATE_SET,
			   UMS9117_AON_SPI1_GATE);
	if (ret)
		return ret;
	writel(3, udrm->spi_clock_selector);
	writel(0, udrm->lcm + 0x000);
	writel(1, udrm->lcm + 0x010);
	writel(0x00a50100, udrm->lcm + 0x014);
	writel(UMS9117_AP_AHB_SPI1_RESET, udrm->spi_reset_set);
	usleep_range(1000, 2000);
	writel(UMS9117_AP_AHB_SPI1_RESET, udrm->spi_reset_clear);
	usleep_range(1000, 2000);
	writel(0, udrm->spi + UMS9117_DRM_SPI_INT_EN);
	writel(0xf00 | 2 | (8 << 2), udrm->spi + UMS9117_DRM_SPI_CTL0);
	value = readl(udrm->spi + UMS9117_DRM_SPI_CTL1);
	writel((value & ~0x3000) | 0x3000, udrm->spi + UMS9117_DRM_SPI_CTL1);
	writel((readl(udrm->spi + UMS9117_DRM_SPI_CTL2) & ~0x1f) | 7,
	       udrm->spi + UMS9117_DRM_SPI_CTL2);
	writel(0x8000, udrm->spi + UMS9117_DRM_SPI_CTL4);
	writel(0, udrm->spi + UMS9117_DRM_SPI_CTL5);
	writel(UMS9117_DRM_SPI_INIT_DIVIDER, udrm->spi + UMS9117_DRM_SPI_CLKD);
	writel(UMS9117_DRM_SPI_MODE_3WIRE_9BIT,
	       udrm->spi + UMS9117_DRM_SPI_CTL7);
	writel(0, udrm->spi + UMS9117_DRM_SPI_CTL8);
	writel(UMS9117_DRM_SPI_TX_END, udrm->spi + UMS9117_DRM_SPI_INT_CLR);
	return 0;
}

int ums9117_drm_spi_dcs(struct ums9117_drm *udrm, u8 command, const u8 *data,
			size_t length)
{
	size_t i;
	int ret;

	ret = ums9117_drm_spi_word(udrm, command, true, false);
	for (i = 0; !ret && i < length; i++)
		ret = ums9117_drm_spi_word(udrm, data[i], false, false);
	ums9117_drm_spi_pixel_mode(udrm);
	return ret;
}

int ums9117_drm_spi_begin_frame(struct ums9117_drm *udrm, u32 pixels)
{
	int ret;

	/* Nokia's proven run path is divider 0 with TX_HOLD before every 2c. */
	writel(0, udrm->spi + UMS9117_DRM_SPI_CLKD);
	writel(readl(udrm->spi + UMS9117_DRM_SPI_CTL7) |
		       UMS9117_DRM_SPI_TX_HOLD,
	       udrm->spi + UMS9117_DRM_SPI_CTL7);
	ret = ums9117_drm_spi_dcs(udrm, MIPI_DCS_WRITE_MEMORY_START, NULL, 0);
	if (!ret) {
		ums9117_drm_spi_tx_length(udrm, pixels);
		writel(readl(udrm->spi + UMS9117_DRM_SPI_CTL12) | BIT(1),
		       udrm->spi + UMS9117_DRM_SPI_CTL12);
	}
	return ret;
}

int ums9117_drm_spi_init_transport(struct ums9117_drm *udrm,
				   struct platform_device *pdev)
{
	return ums9117_drm_spi_init(udrm, pdev);
}

int ums9117_drm_spi_enable_transport(struct ums9117_drm *udrm)
{
	return ums9117_drm_spi_enable(udrm);
}

int ums9117_drm_spi_post_reset(struct ums9117_drm *udrm)
{
	/* Select the panel chip before the DCS init sequence runs. */
	writel(readl(udrm->spi + UMS9117_DRM_SPI_CTL0) & ~UMS9117_DRM_SPI_CS0,
	       udrm->spi + UMS9117_DRM_SPI_CTL0);
	return 0;
}
