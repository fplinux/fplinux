// SPDX-License-Identifier: GPL-2.0-only
/* UMS9117 LCM/DBI CS0 16-bit panel transport. */
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <video/mipi_display.h>

#include "ums9117-drm-internal.h"

#define UMS9117_DRM_LCM_CTRL 0x000
#define UMS9117_DRM_LCM_STATUS 0x008
#define UMS9117_DRM_LCM_CS0_MODE 0x010
#define UMS9117_DRM_LCM_CS0_TIMING 0x014
#define UMS9117_DRM_LCM_STATUS_WRITE_BUSY BIT(1)
#define UMS9117_DRM_LCM_IDLE_TIMEOUT_US 250000
#define UMS9117_DRM_LCM_AHB_MHZ 128

/* fpdoom's `DBI_CYCLES`: ceil(clock MHz * nanoseconds / 1000), clamped. */
static u32 ums9117_drm_lcm_dbi_cycles(u32 ns, u32 max)
{
	u32 cycles = DIV_ROUND_UP(UMS9117_DRM_LCM_AHB_MHZ * ns, 1000);

	return min(cycles, max);
}

/*
 * This follows fpdoom 9695f373's lcm_set_freq() bit-for-bit.  It is kept as
 * a computation because DT owns the target timing tuple, not a packed value.
 */
static u32 ums9117_drm_lcm_dbi_timing(const u32 ns[6])
{
	u32 rcss = ums9117_drm_lcm_dbi_cycles(ns[0], 6);
	u32 rlpw = ums9117_drm_lcm_dbi_cycles(ns[1], 14);
	u32 rhpw = ums9117_drm_lcm_dbi_cycles(ns[2], 14);
	u32 wcss = ums9117_drm_lcm_dbi_cycles(ns[3], 6);
	u32 wlpw = ums9117_drm_lcm_dbi_cycles(ns[4], 14);
	u32 whpw = ums9117_drm_lcm_dbi_cycles(ns[5], 14);
	u32 read_total = min(rcss + rlpw + rhpw, 30U);
	u32 write_total;

	/* The controller's write total includes max(whpw - 1, wcss + 1). */
	write_total = wlpw + max(whpw - 1, wcss + 1);
	return (write_total << 21) | (read_total << 16) | (wlpw << 8) |
	       (wcss << 4) | rcss | BIT(7);
}

static int ums9117_drm_lcm_wait_idle(struct ums9117_drm *udrm)
{
	u32 status;

	return readl_poll_timeout_atomic(
		udrm->lcm + UMS9117_DRM_LCM_STATUS, status,
		!(status & UMS9117_DRM_LCM_STATUS_WRITE_BUSY), 1,
		UMS9117_DRM_LCM_IDLE_TIMEOUT_US);
}

static int ums9117_drm_lcm_init(struct ums9117_drm *udrm,
				struct platform_device *pdev)
{
	u32 timings[6];
	int ret;

	udrm->lcm = devm_platform_ioremap_resource_byname(pdev, "lcm");
	if (IS_ERR(udrm->lcm))
		return PTR_ERR(udrm->lcm);
	udrm->lcm_command =
		devm_platform_ioremap_resource_byname(pdev, "lcm-command");
	if (IS_ERR(udrm->lcm_command))
		return PTR_ERR(udrm->lcm_command);
	udrm->lcm_data =
		devm_platform_ioremap_resource_byname(pdev, "lcm-data");
	if (IS_ERR(udrm->lcm_data))
		return PTR_ERR(udrm->lcm_data);
	ret = of_property_read_u32_array(pdev->dev.of_node,
					 "sprd,dbi-timing-ns", timings,
					 ARRAY_SIZE(timings));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "six DBI timing values are required\n");
	udrm->lcm_timing = ums9117_drm_lcm_dbi_timing(timings);
	udrm->stream_phys =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "lcm-data")
			->start;
	return 0;
}

static int ums9117_drm_lcm_enable(struct ums9117_drm *udrm)
{
	int ret = ums9117_drm_lcm_wait_idle(udrm);

	if (ret)
		return ret;
	writel(0, udrm->lcm + UMS9117_DRM_LCM_CTRL);
	writel(1, udrm->lcm + UMS9117_DRM_LCM_CS0_MODE);
	/* Keep the controller in fpdoom's generic-safe state during reset. */
	writel(0x00a50100, udrm->lcm + UMS9117_DRM_LCM_CS0_TIMING);
	return ums9117_drm_lcm_wait_idle(udrm);
}

static int ums9117_drm_lcm_program_after_reset(struct ums9117_drm *udrm)
{
	int ret = ums9117_drm_lcm_wait_idle(udrm);

	if (ret)
		return ret;
	/* fpdoom reinstalls safe state, then programs CTRL and final timing. */
	writel(0, udrm->lcm + UMS9117_DRM_LCM_CTRL);
	writel(1, udrm->lcm + UMS9117_DRM_LCM_CS0_MODE);
	writel(0x00a50100, udrm->lcm + UMS9117_DRM_LCM_CS0_TIMING);
	writel(0x11110000, udrm->lcm + UMS9117_DRM_LCM_CTRL);
	ret = ums9117_drm_lcm_wait_idle(udrm);
	if (ret)
		return ret;
	writel(udrm->lcm_timing, udrm->lcm + UMS9117_DRM_LCM_CS0_TIMING);
	return ums9117_drm_lcm_wait_idle(udrm);
}

int ums9117_drm_lcm_dcs(struct ums9117_drm *udrm, u8 command, const u8 *data,
			size_t length)
{
	size_t i;
	int ret;

	ret = ums9117_drm_lcm_wait_idle(udrm);
	if (ret)
		return ret;
	writel(1, udrm->lcm + UMS9117_DRM_LCM_CS0_MODE);
	ret = ums9117_drm_lcm_wait_idle(udrm);
	if (ret)
		return ret;
	writew(command, udrm->lcm_command);
	for (i = 0; i < length; i++) {
		ret = ums9117_drm_lcm_wait_idle(udrm);
		if (ret)
			return ret;
		writew(data[i], udrm->lcm_data);
	}
	return ums9117_drm_lcm_wait_idle(udrm);
}

int ums9117_drm_lcm_begin_frame(struct ums9117_drm *udrm)
{
	int ret;

	ret = ums9117_drm_lcm_wait_idle(udrm);
	if (ret)
		return ret;
	writel(1, udrm->lcm + UMS9117_DRM_LCM_CS0_MODE);
	ret = ums9117_drm_lcm_wait_idle(udrm);
	if (ret)
		return ret;
	writew(MIPI_DCS_WRITE_MEMORY_START, udrm->lcm_command);
	ret = ums9117_drm_lcm_wait_idle(udrm);
	if (!ret)
		writel(0x28, udrm->lcm + UMS9117_DRM_LCM_CS0_MODE);
	return ret;
}

int ums9117_drm_lcm_init_transport(struct ums9117_drm *udrm,
				   struct platform_device *pdev)
{
	return ums9117_drm_lcm_init(udrm, pdev);
}

int ums9117_drm_lcm_enable_transport(struct ums9117_drm *udrm)
{
	return ums9117_drm_lcm_enable(udrm);
}

int ums9117_drm_lcm_post_reset(struct ums9117_drm *udrm)
{
	return ums9117_drm_lcm_program_after_reset(udrm);
}
