// SPDX-License-Identifier: GPL-2.0-only
/* UNIWA V1000 4G panel profile for the common UMS9117 DRM core. */
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"

#include "fplinux-uniwa-v1000-4g-panel.h"

static const struct ums9117_drm_profile uniwa_v1000_drm_profile = {
	.name = "uniwa-v1000-rgb565",
	.transport = FPLINUX_PANEL_TRANSPORT,
	.init = fplinux_panel_init,
	.init_count = ARRAY_SIZE(fplinux_panel_init),
	.width = FPLINUX_PANEL_WIDTH,
	.height = FPLINUX_PANEL_HEIGHT,
	.reset_phase_ms = 16,
	.sleep_out_ms = 120,
	.spi_frame_clock_divider = 1,
	.spi_init_clock_divider = 1,
	.spi_native_16bit = true,
	.wled_backlight_name = "uniwa-v1000-backlight",
	.native_nv16 = true,
	.lcdc_ctrl_set = BIT(1),
	.lcdc_ctrl_clear = BIT(2) | (7U << 5),
};

static const struct of_device_id uniwa_v1000_drm_of_match[] = {
	{ .compatible = "fplinux,ums9117-uniwa-v1000-spi-fb",
	  .data = &uniwa_v1000_drm_profile },
	{}
};
MODULE_DEVICE_TABLE(of, uniwa_v1000_drm_of_match);

static struct platform_driver uniwa_v1000_drm_driver = {
	.probe = ums9117_drm_probe,
	.remove = ums9117_drm_remove,
	.shutdown = ums9117_drm_shutdown,
	.driver = {
		.name = "uniwa-v1000-drm",
		.of_match_table = uniwa_v1000_drm_of_match,
		.pm = pm_sleep_ptr(&ums9117_drm_pm_ops),
	},
};
module_platform_driver(uniwa_v1000_drm_driver);

MODULE_DESCRIPTION("UNIWA V1000 4G panel profile");
MODULE_LICENSE("GPL");
