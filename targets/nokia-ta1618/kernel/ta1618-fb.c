// SPDX-License-Identifier: GPL-2.0-only
/* Nokia TA-1618 ST7789P3 profile for the common UMS9117 fbdev core. */
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-fb.h"

static const struct ums9117_fb_command ta1618_panel_init[] = {
	{ 0x11, 0, 120, {} },
	{ 0xb2, 5, 0, { 0x0c, 0x0c, 0x00, 0x33, 0x33 } },
	{ 0x35, 1, 0, { 0x00 } },
	{ 0x3a, 1, 0, { 0x05 } },
	{ 0xb7, 1, 0, { 0x56 } },
	{ 0xbb, 1, 0, { 0x0c } },
	{ 0xc0, 1, 0, { 0x2c } },
	{ 0xc2, 1, 0, { 0x01 } },
	{ 0xc3, 1, 0, { 0x0f } },
	{ 0xc6, 1, 0, { 0x0f } },
	{ 0xd0, 1, 10, { 0xa7 } },
	{ 0xd0, 2, 0, { 0xa4, 0xa1 } },
	{ 0xd6, 1, 0, { 0xa1 } },
	{ 0xe0,
	  14,
	  0,
	  { 0xf0, 0x01, 0x08, 0x04, 0x05, 0x14, 0x33, 0x44, 0x49, 0x36, 0x11,
	    0x14, 0x2e, 0x36 } },
	{ 0xe1,
	  14,
	  0,
	  { 0xf0, 0x0c, 0x10, 0x0e, 0x0c, 0x08, 0x32, 0x43, 0x49, 0x28, 0x12,
	    0x12, 0x2c, 0x33 } },
	{ 0x21, 0, 0, {} },
	{ 0x29, 0, 0, {} },
	{ 0xc6, 1, 0, { 0x18 } },
	{ 0x36, 1, 0, { 0x00 } },
	{ 0x2a, 4, 0, { 0x00, 0x00, 0x00, 0xef } },
	{ 0x2b, 4, 0, { 0x00, 0x00, 0x01, 0x3f } },
};

static const struct ums9117_fb_profile ta1618_fb_profile = {
	.name = "ta1618-rgb565",
	.transport = UMS9117_FB_TRANSPORT_SPI1_3WIRE,
	.completion = UMS9117_FB_COMPLETION_IRQ,
	.init = ta1618_panel_init,
	.init_count = ARRAY_SIZE(ta1618_panel_init),
	.width = 240,
	.height = 320,
	.reset_phase_ms = 10,
	.reset_release_ms = 120,
	.sleep_in_ms = 5,
	.sleep_out_ms = 120,
	.wled_backlight_name = "ta1618-backlight",
	.lcdc_ctrl_set = BIT(2),
	.lcdc_ctrl_clear = BIT(1) | (7U << 5),
};

static int ta1618_fb_probe(struct platform_device *pdev)
{
	return ums9117_fb_probe(pdev, &ta1618_fb_profile);
}

static const struct of_device_id ta1618_fb_of_match[] = {
	{ .compatible = "fplinux,ums9117-st7789p3-spi-fb" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_fb_of_match);

static struct platform_driver ta1618_fb_driver = {
	.probe = ta1618_fb_probe,
	.remove = ums9117_fb_remove,
	.shutdown = ums9117_fb_shutdown,
	.driver = {
		.name = "ta1618-fb",
		.of_match_table = ta1618_fb_of_match,
		.pm = pm_sleep_ptr(&ums9117_fb_pm_ops),
	},
};
module_platform_driver(ta1618_fb_driver);

MODULE_DESCRIPTION("Nokia TA-1618 ST7789P3 panel profile");
MODULE_LICENSE("GPL");
