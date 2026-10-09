// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"

static const struct ums9117_drm_command bq2830l_panel_init[] = {
	{ 0x00, 0, 120, {} },
	{ 0xfe, 0, 0, {} },
	{ 0xef, 0, 0, {} },
	{ 0x36, 1, 0, { 0x48 } },
	{ 0x3a, 1, 0, { 0x05 } },
	{ 0x21, 0, 0, {} },
	{ 0x86, 1, 0, { 0x98 } },
	{ 0x89, 1, 0, { 0x33 } },
	{ 0x8b, 1, 0, { 0x80 } },
	{ 0x8d, 1, 0, { 0x33 } },
	{ 0x8e, 1, 0, { 0x0f } },
	{ 0xe8, 2, 0, { 0x13, 0x55 } },
	{ 0xec, 3, 0, { 0x33, 0x06, 0x00 } },
	{ 0xed, 2, 0, { 0x18, 0x09 } },
	{ 0xff, 1, 0, { 0x62 } },
	{ 0x99, 1, 0, { 0x3e } },
	{ 0x9d, 1, 0, { 0x4b } },
	{ 0x98, 1, 0, { 0x3e } },
	{ 0x9c, 1, 0, { 0x4b } },
	{ 0xc3, 1, 0, { 0x1a } },
	{ 0xc4, 1, 0, { 0x10 } },
	{ 0xc9, 1, 0, { 0x18 } },
	{ 0xf0, 6, 0, { 0x82, 0x00, 0x16, 0x15, 0x0b, 0x3c } },
	{ 0xf1, 6, 0, { 0x4d, 0x98, 0x98, 0x20, 0x2c, 0xbf } },
	{ 0xf2, 6, 0, { 0x42, 0x09, 0x0d, 0x0e, 0x0a, 0x3b } },
	{ 0xf3, 6, 0, { 0x4d, 0x93, 0x91, 0x20, 0x1a, 0x9f } },
	{ 0x35, 1, 0, { 0x00 } },
	{ 0x44, 2, 0, { 0x00, 0x0a } },
};

static const struct ums9117_drm_command bq2830l_panel_init_finish[] = {
	{ 0x11, 0, 120, {} },
	{ 0x29, 0, 0, {} },
	{ 0x2c, 0, 0, {} },
};

static const struct ums9117_drm_profile bq2830l_drm_profile = {
	.name = "bq-2830l-rgb565",
	.transport = UMS9117_DRM_TRANSPORT_SPI1_3WIRE,
	.init = bq2830l_panel_init,
	.init_count = ARRAY_SIZE(bq2830l_panel_init),
	.init_finish = bq2830l_panel_init_finish,
	.init_finish_count = ARRAY_SIZE(bq2830l_panel_init_finish),
	.wake_finish = bq2830l_panel_init_finish,
	.wake_finish_count = ARRAY_SIZE(bq2830l_panel_init_finish),
	.width = 240,
	.height = 320,
	.reset_phase_ms = 16,
	.reset_release_ms = 0,
	.wake_reset_phase_ms = 16,
	.display_off_ms = 120,
	.sleep_in_ms = 200,
	.spi_frame_clock_divider = 1,
	.wled_backlight_name = "bq-2830l-backlight",
	.native_nv16 = true,
	.lcdc_ctrl_set = BIT(1),
	.lcdc_ctrl_clear = BIT(2) | (7U << 5),
};

static const struct of_device_id bq2830l_drm_of_match[] = {
	{ .compatible = "fplinux,ums9117-gc9307-spi-fb",
	  .data = &bq2830l_drm_profile },
	{}
};
MODULE_DEVICE_TABLE(of, bq2830l_drm_of_match);

static struct platform_driver bq2830l_drm_driver = {
	.probe = ums9117_drm_probe,
	.remove = ums9117_drm_remove,
	.shutdown = ums9117_drm_shutdown,
	.driver = {
		.name = "bq-2830l-drm",
		.of_match_table = bq2830l_drm_of_match,
		.pm = pm_sleep_ptr(&ums9117_drm_pm_ops),
	},
};
module_platform_driver(bq2830l_drm_driver);

MODULE_DESCRIPTION("BQ 2830L Tank Connect Max GC9307 panel profile");
MODULE_LICENSE("GPL");
