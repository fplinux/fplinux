// SPDX-License-Identifier: GPL-2.0-only
/* INOI 244 Modern 4G NV3030 profile for the common UMS9117 DRM core. */
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"

#include "fplinux-inoi-244-modern-4g-panel.h"

static const struct ums9117_drm_profile inoi244_drm_profile = {
	.name = "inoi244-rgb565",
	.transport = FPLINUX_PANEL_TRANSPORT,
	.init = fplinux_panel_init,
	.init_count = ARRAY_SIZE(fplinux_panel_init),
	.init_finish = fplinux_panel_cold_finish,
	.init_finish_count = ARRAY_SIZE(fplinux_panel_cold_finish),
	.wake_finish = fplinux_panel_wake_finish,
	.wake_finish_count = ARRAY_SIZE(fplinux_panel_wake_finish),
	.width = FPLINUX_PANEL_WIDTH,
	.height = FPLINUX_PANEL_HEIGHT,
	.reset_phase_ms = 10,
	.reset_release_ms = 0,
	.wake_reset_phase_ms = 16,
	.sleep_in_ms = 5,
	.sleep_out_ms = 200,
	.wled_backlight_name = "inoi244-backlight",
	.native_nv16 = true,
	.lcdc_ctrl_set = BIT(1),
	.lcdc_ctrl_clear = BIT(2) | (7U << 5),
};

static const struct of_device_id inoi244_drm_of_match[] = {
	{ .compatible = "fplinux,ums9117-nv3030-lcm-fb",
	  .data = &inoi244_drm_profile },
	{}
};
MODULE_DEVICE_TABLE(of, inoi244_drm_of_match);

static struct platform_driver inoi244_drm_driver = {
	.probe = ums9117_drm_probe,
	.remove = ums9117_drm_remove,
	.shutdown = ums9117_drm_shutdown,
	.driver = {
		.name = "inoi244-drm",
		.of_match_table = inoi244_drm_of_match,
		.pm = &ums9117_drm_pm_ops,
	},
};
module_platform_driver(inoi244_drm_driver);

MODULE_DESCRIPTION("INOI 244 Modern 4G NV3030 panel profile");
MODULE_LICENSE("GPL");
