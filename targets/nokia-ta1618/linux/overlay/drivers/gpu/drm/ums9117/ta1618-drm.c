// SPDX-License-Identifier: GPL-2.0-only
/* Nokia TA-1618 ST7789P3 profile for the common UMS9117 DRM core. */
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"

#include "fplinux-nokia-ta1618-panel.h"

static const struct ums9117_drm_profile ta1618_drm_profile = {
	.name = "ta1618-rgb565",
	.transport = FPLINUX_PANEL_TRANSPORT,
	.init = fplinux_panel_init,
	.init_count = ARRAY_SIZE(fplinux_panel_init),
	.width = FPLINUX_PANEL_WIDTH,
	.height = FPLINUX_PANEL_HEIGHT,
	.reset_phase_ms = 10,
	.reset_release_ms = 120,
	.sleep_in_ms = 5,
	.sleep_out_ms = 120,
	.spi_frame_clock_divider = 0,
	.wled_backlight_name = "ta1618-backlight",
	.lcdc_ctrl_set = BIT(2),
	.lcdc_ctrl_clear = BIT(1) | (7U << 5),
};

static const struct of_device_id ta1618_drm_of_match[] = {
	{ .compatible = "fplinux,ums9117-st7789p3-spi-fb",
	  .data = &ta1618_drm_profile },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_drm_of_match);

static struct platform_driver ta1618_drm_driver = {
	.probe = ums9117_drm_probe,
	.remove = ums9117_drm_remove,
	.shutdown = ums9117_drm_shutdown,
	.driver = {
		.name = "ta1618-drm",
		.of_match_table = ta1618_drm_of_match,
		.pm = pm_sleep_ptr(&ums9117_drm_pm_ops),
	},
};
module_platform_driver(ta1618_drm_driver);

MODULE_DESCRIPTION("Nokia TA-1618 ST7789P3 panel profile");
MODULE_LICENSE("GPL");
