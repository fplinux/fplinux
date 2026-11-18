// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"
#include "fplinux-konrow-senior-280-plus-panel.h"

static const struct ums9117_drm_profile konrow_senior_280_plus_drm_profile = {
	.name = "konrow-senior-280-plus-rgb565",
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
	.wake_reset_phase_ms = 16,
	.sleep_in_ms = 30,
	.wled_backlight_name = "konrow-senior-280-plus-backlight",
	.wled_mode = UMS9117_DRM_WLED_WHITE_PWM,
	.lcdc_ctrl_set = BIT(1),
	.lcdc_ctrl_clear = BIT(2) | (7U << 5),
};

static const struct of_device_id konrow_senior_280_plus_drm_of_match[] = {
	{ .compatible = "fplinux,ums9117-konrow-senior-280-plus-lcm-fb",
	  .data = &konrow_senior_280_plus_drm_profile },
	{}
};
MODULE_DEVICE_TABLE(of, konrow_senior_280_plus_drm_of_match);

static struct platform_driver konrow_senior_280_plus_drm_driver = {
	.probe = ums9117_drm_probe,
	.remove = ums9117_drm_remove,
	.shutdown = ums9117_drm_shutdown,
	.driver = {
		.name = "konrow-senior-280-plus-drm",
		.of_match_table = konrow_senior_280_plus_drm_of_match,
		.pm = pm_sleep_ptr(&ums9117_drm_pm_ops),
	},
};
module_platform_driver(konrow_senior_280_plus_drm_driver);

MODULE_DESCRIPTION("Konrow Senior 280 Plus panel profile");
MODULE_LICENSE("GPL");
