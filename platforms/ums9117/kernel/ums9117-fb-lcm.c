// SPDX-License-Identifier: GPL-2.0-only
/* UMS9117 LCM/DBI CS0 16-bit panel transport. */
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/of.h>

#include "ums9117-fb-internal.h"

#define LCM_CTRL 0x000
#define LCM_CS0_MODE 0x010
#define LCM_CS0_TIMING 0x014
#define LCM_BUSY BIT(1)
#define LCM_IDLE_TIMEOUT_US 250000
#define LCM_AHB_MHZ 128

/* fpdoom's `DBI_CYCLES`: ceil(clock MHz * nanoseconds / 1000), clamped. */
static u32 ums9117_dbi_cycles(u32 ns, u32 max)
{
	u32 cycles = DIV_ROUND_UP(LCM_AHB_MHZ * ns, 1000);

	return min(cycles, max);
}

/*
 * This follows fpdoom 9695f373's lcm_set_freq() bit-for-bit.  It is kept as
 * a computation because DT owns the target timing tuple, not a packed value.
 */
static u32 ums9117_dbi_timing(const u32 ns[6])
{
	u32 rcss = ums9117_dbi_cycles(ns[0], 6);
	u32 rlpw = ums9117_dbi_cycles(ns[1], 14);
	u32 rhpw = ums9117_dbi_cycles(ns[2], 14);
	u32 wcss = ums9117_dbi_cycles(ns[3], 6);
	u32 wlpw = ums9117_dbi_cycles(ns[4], 14);
	u32 whpw = ums9117_dbi_cycles(ns[5], 14);
	u32 read_total = min(rcss + rlpw + rhpw, 30U);
	u32 write_total;

	/* The controller's write total includes max(whpw - 1, wcss + 1). */
	write_total = wlpw + max(whpw - 1, wcss + 1);
	return (write_total << 21) | (read_total << 16) | (wlpw << 8) |
	       (wcss << 4) | rcss | BIT(7);
}

static int ums9117_lcm_wait_idle(struct ums9117_fb *ufb)
{
	u32 status;

	return readl_poll_timeout_atomic(ufb->lcm + LCM_CTRL, status,
					 !(status & LCM_BUSY), 1,
					 LCM_IDLE_TIMEOUT_US);
}

static int ums9117_lcm_init(struct ums9117_fb *ufb,
			    struct platform_device *pdev)
{
	u32 timings[6];
	u32 packed;
	int ret;

	ufb->lcm = devm_platform_ioremap_resource_byname(pdev, "lcm");
	if (IS_ERR(ufb->lcm))
		return PTR_ERR(ufb->lcm);
	ufb->lcm_command =
		devm_platform_ioremap_resource_byname(pdev, "lcm-command");
	if (IS_ERR(ufb->lcm_command))
		return PTR_ERR(ufb->lcm_command);
	ufb->lcm_data = devm_platform_ioremap_resource_byname(pdev, "lcm-data");
	if (IS_ERR(ufb->lcm_data))
		return PTR_ERR(ufb->lcm_data);
	ret = of_property_read_u32_array(pdev->dev.of_node,
					 "sprd,dbi-timing-ns", timings,
					 ARRAY_SIZE(timings));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "six DBI timing values are required\n");
	packed = ums9117_dbi_timing(timings);
	/* INOI 244 Modern 4G's source-backed tuple validates this calculation. */
	if (timings[0] == 5 && timings[1] == 150 && timings[2] == 150 &&
	    timings[3] == 30 && timings[4] == 80 && timings[5] == 120 &&
	    WARN_ON_ONCE(packed != 0x031d0bc1))
		return -EINVAL;
	ufb->lcm_timing = packed;
	ufb->stream_phys =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "lcm-data")
			->start;
	return 0;
}

static int ums9117_lcm_enable(struct ums9117_fb *ufb)
{
	int ret = ums9117_lcm_wait_idle(ufb);

	if (ret)
		return ret;
	writel(0, ufb->lcm + LCM_CTRL);
	writel(1, ufb->lcm + LCM_CS0_MODE);
	/* Keep the controller in fpdoom's generic-safe state during reset. */
	writel(0x00a50100, ufb->lcm + LCM_CS0_TIMING);
	return ums9117_lcm_wait_idle(ufb);
}

static int ums9117_lcm_post_reset(struct ums9117_fb *ufb)
{
	int ret = ums9117_lcm_wait_idle(ufb);

	if (ret)
		return ret;
	/* fpdoom reinstalls safe state, then programs CTRL and final timing. */
	writel(0, ufb->lcm + LCM_CTRL);
	writel(1, ufb->lcm + LCM_CS0_MODE);
	writel(0x00a50100, ufb->lcm + LCM_CS0_TIMING);
	writel(0x11110000, ufb->lcm + LCM_CTRL);
	ret = ums9117_lcm_wait_idle(ufb);
	if (ret)
		return ret;
	writel(ufb->lcm_timing, ufb->lcm + LCM_CS0_TIMING);
	return ums9117_lcm_wait_idle(ufb);
}

int ums9117_fb_lcm_dcs(struct ums9117_fb *ufb, u8 command, const u8 *data,
		       size_t length)
{
	size_t i;
	int ret;

	ret = ums9117_lcm_wait_idle(ufb);
	if (ret)
		return ret;
	writel(1, ufb->lcm + LCM_CS0_MODE);
	ret = ums9117_lcm_wait_idle(ufb);
	if (ret)
		return ret;
	writew(command, ufb->lcm_command);
	for (i = 0; i < length; i++) {
		ret = ums9117_lcm_wait_idle(ufb);
		if (ret)
			return ret;
		writew(data[i], ufb->lcm_data);
	}
	return ums9117_lcm_wait_idle(ufb);
}

int ums9117_fb_lcm_begin_frame(struct ums9117_fb *ufb)
{
	int ret;

	ret = ums9117_lcm_wait_idle(ufb);
	if (ret)
		return ret;
	writel(1, ufb->lcm + LCM_CS0_MODE);
	ret = ums9117_lcm_wait_idle(ufb);
	if (ret)
		return ret;
	writew(0x2c, ufb->lcm_command);
	ret = ums9117_lcm_wait_idle(ufb);
	if (!ret)
		writel(0x28, ufb->lcm + LCM_CS0_MODE);
	return ret;
}

int ums9117_fb_lcm_init_transport(struct ums9117_fb *ufb,
				  struct platform_device *pdev)
{
	return ums9117_lcm_init(ufb, pdev);
}

int ums9117_fb_lcm_enable_transport(struct ums9117_fb *ufb)
{
	return ums9117_lcm_enable(ufb);
}

int ums9117_fb_lcm_post_reset(struct ums9117_fb *ufb)
{
	return ums9117_lcm_post_reset(ufb);
}

u32 ums9117_fb_lcm_dbi_timing_for_test(const u32 ns[6])
{
	return ums9117_dbi_timing(ns);
}
