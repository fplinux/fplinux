// SPDX-License-Identifier: GPL-2.0-only
/* UNIWA V1000 4G panel profile for the common UMS9117 DRM core. */
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"

static const struct ums9117_drm_command uniwa_v1000_panel_init[] = {
	{ 0x00, 0, 10, {} },
	{ 0xc8, 3, 0, { 0xff, 0x93, 0x42 } },
	{ 0x36, 1, 0, { 0x08 } },
	{ 0x35, 1, 0, { 0x00 } },
	{ 0x3a, 1, 0, { 0x55 } },
	{ 0xc0, 2, 0, { 0x13, 0x0e } },
	{ 0xc1, 1, 0, { 0x02 } },
	{ 0xc5, 1, 0, { 0xd3 } },
	{ 0xb1, 2, 0, { 0x00, 0x1b } },
	{ 0xb4, 1, 0, { 0x02 } },
	{ 0xb6, 2, 0, { 0x0a, 0xe0 } },
	{ 0xe0,
	  15,
	  0,
	  { 0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a,
	    0x08, 0x17, 0x17, 0x0f } },
	{ 0xe1,
	  15,
	  0,
	  { 0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f,
	    0x0e, 0x37, 0x38, 0x0f } },
	{ 0x11, 0, 120, {} },
	{ 0x29, 0, 20, {} },
	{ 0x2c, 0, 0, {} },
	{ 0x36, 1, 0, { 0x08 } },
	{ 0x2a, 4, 0, { 0x00, 0x00, 0x01, 0x3f } },
	{ 0x2b, 4, 0, { 0x00, 0x00, 0x00, 0xef } },
};

static const struct ums9117_drm_profile uniwa_v1000_drm_profile = {
	.name = "uniwa-v1000-rgb565",
	.transport = UMS9117_DRM_TRANSPORT_SPI1_3WIRE,
	.init = uniwa_v1000_panel_init,
	.init_count = ARRAY_SIZE(uniwa_v1000_panel_init),
	.width = 320,
	.height = 240,
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
