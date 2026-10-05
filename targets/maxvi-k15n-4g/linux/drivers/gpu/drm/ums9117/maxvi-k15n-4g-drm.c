// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"

static const struct ums9117_drm_command maxvi_panel_init[] = {
	{ 0xfe, 0, 0, {} },
	{ 0xef, 0, 0, {} },
	{ 0x36, 1, 0, { 0x48 } },
	{ 0x3a, 1, 0, { 0x05 } },
	{ 0x21, 0, 0, {} },
	{ 0x86, 1, 0, { 0x98 } },
	{ 0x89, 1, 0, { 0x33 } },
	{ 0x8b, 1, 0, { 0x84 } },
	{ 0x8d, 1, 0, { 0x33 } },
	{ 0x8e, 1, 0, { 0x0f } },
	{ 0xe8, 2, 0, { 0x12, 0x00 } },
	{ 0xec, 3, 0, { 0x33, 0x07, 0x01 } },
	{ 0xff, 1, 0, { 0x62 } },
	{ 0x99, 1, 0, { 0x3e } },
	{ 0x9d, 1, 0, { 0x4b } },
	{ 0x98, 1, 0, { 0x3e } },
	{ 0x9c, 1, 0, { 0x4b } },
	{ 0xc3, 1, 0, { 0x1c } },
	{ 0xc4, 1, 0, { 0x08 } },
	{ 0xc9, 1, 0, { 0x10 } },
	{ 0xf0, 6, 0, { 0x88, 0x00, 0x1f, 0x16, 0x0b, 0x37 } },
	{ 0xf1, 6, 0, { 0x4c, 0xb7, 0x97, 0x22, 0x23, 0xbf } },
	{ 0xf2, 6, 0, { 0x4b, 0x00, 0x00, 0x19, 0x0d, 0x35 } },
	{ 0xf3, 6, 0, { 0x47, 0xf5, 0xb4, 0x1a, 0x1c, 0xcf } },
	{ 0x35, 1, 0, { 0x00 } },
	{ 0x44, 2, 0, { 0x00, 0x0a } },
};

/* Cold start and wake both restore the complete panel state. */
static const struct ums9117_drm_command maxvi_panel_init_finish[] = {
	{ 0x11, 0, 120, {} },
	{ 0x29, 0, 0, {} },
	{ 0x2c, 0, 0, {} },
	{ 0x2c, 0, 0, {} },
};

static const struct ums9117_drm_profile maxvi_drm_profile = {
	.name = "maxvi-k15n-4g-rgb565",
	.transport = UMS9117_DRM_TRANSPORT_LCM_DBI,
	.init = maxvi_panel_init,
	.init_count = ARRAY_SIZE(maxvi_panel_init),
	.init_finish = maxvi_panel_init_finish,
	.init_finish_count = ARRAY_SIZE(maxvi_panel_init_finish),
	.wake_finish = maxvi_panel_init_finish,
	.wake_finish_count = ARRAY_SIZE(maxvi_panel_init_finish),
	.width = 240,
	.height = 320,
	.reset_phase_ms = 16,
	.reset_release_ms = 0,
	.wake_reset_phase_ms = 16,
	.display_off_ms = 120,
	.sleep_in_ms = 50,
	.sleep_out_ms = 120,
	.wled_backlight_name = "maxvi-k15n-4g-backlight",
	.native_nv16 = true,
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
