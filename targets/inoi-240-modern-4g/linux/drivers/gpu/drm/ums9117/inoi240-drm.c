// SPDX-License-Identifier: GPL-2.0-only
/* INOI 240 Modern 4G NV3023 profile for the common UMS9117 DRM core. */
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"

#include "fplinux-inoi-240-modern-4g-panel.h"

static const struct ums9117_drm_profile inoi240_drm_profile = {
	.name = "inoi240-rgb565",
	.transport = FPLINUX_PANEL_TRANSPORT,
	.init = fplinux_panel_init,
	.init_count = ARRAY_SIZE(fplinux_panel_init),
	.width = FPLINUX_PANEL_WIDTH,
	.height = FPLINUX_PANEL_HEIGHT,
	.reset_phase_ms = 10,
	.reset_release_ms = 0,
	.sleep_in_ms = 5,
	.sleep_out_ms = 200,
	.wled_backlight_name = "inoi240-backlight",
	.native_nv16 = true,
	.lcdc_ctrl_set = BIT(1),
	.lcdc_ctrl_clear = BIT(2) | (7U << 5),
};

static const struct of_device_id inoi240_drm_of_match[] = {
	{ .compatible = "fplinux,ums9117-nv3023-lcm-fb",
	  .data = &inoi240_drm_profile },
	{}
};
MODULE_DEVICE_TABLE(of, inoi240_drm_of_match);

static struct platform_driver inoi240_drm_driver = {
	.probe = ums9117_drm_probe,
	.remove = ums9117_drm_remove,
	.shutdown = ums9117_drm_shutdown,
	.driver = {
		.name = "inoi240-drm",
		.of_match_table = inoi240_drm_of_match,
		.pm = &ums9117_drm_pm_ops,
	},
};
module_platform_driver(inoi240_drm_driver);

MODULE_DESCRIPTION("INOI 240 Modern 4G NV3023 panel profile");
MODULE_LICENSE("GPL");
