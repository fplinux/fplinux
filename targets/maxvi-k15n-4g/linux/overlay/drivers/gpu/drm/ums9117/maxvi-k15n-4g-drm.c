// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"

#include "fplinux-maxvi-k15n-4g-panel.h"

static const struct ums9117_drm_profile maxvi_drm_profile = {
	.name = "maxvi-k15n-4g-rgb565",
	.transport = FPLINUX_PANEL_TRANSPORT,
	.init = fplinux_panel_init,
	.init_count = ARRAY_SIZE(fplinux_panel_init),
	.init_finish = fplinux_panel_cold_finish,
	.init_finish_count = ARRAY_SIZE(fplinux_panel_cold_finish),
	.wake_finish = fplinux_panel_wake_finish,
	.wake_finish_count = ARRAY_SIZE(fplinux_panel_wake_finish),
	.width = FPLINUX_PANEL_WIDTH,
	.height = FPLINUX_PANEL_HEIGHT,
	.reset_phase_ms = 16,
	.reset_release_ms = 0,
	.wake_reset_phase_ms = 16,
	.display_off_ms = 120,
	.sleep_in_ms = 50,
	.sleep_out_ms = 120,
	.wled_backlight_name = "maxvi-k15n-4g-backlight",
	.lcdc_ctrl_set = BIT(1),
	.lcdc_ctrl_clear = BIT(2) | (7U << 5),
};

static const struct of_device_id maxvi_drm_of_match[] = {
	{ .compatible = "fplinux,ums9117-gc9307-lcm-fb",
	  .data = &maxvi_drm_profile },
	{}
};
MODULE_DEVICE_TABLE(of, maxvi_drm_of_match);

static struct platform_driver maxvi_drm_driver = {
	.probe = ums9117_drm_probe,
	.remove = ums9117_drm_remove,
	.shutdown = ums9117_drm_shutdown,
	.driver = {
		.name = "maxvi-k15n-4g-drm",
		.of_match_table = maxvi_drm_of_match,
		.pm = pm_sleep_ptr(&ums9117_drm_pm_ops),
	},
};
module_platform_driver(maxvi_drm_driver);

MODULE_DESCRIPTION("Maxvi K15n4G panel profile");
MODULE_LICENSE("GPL");
