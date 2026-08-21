// SPDX-License-Identifier: GPL-2.0-only
/* INOI 244 Modern 4G NV3030 profile for the common UMS9117 fbdev core. */
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-fb.h"

/* Exact LCM/DBI sequence from fpdoom model 104 (commit 9695f3739639). */
static const struct ums9117_fb_command inoi244_panel_init[] = {
	/* Exact cmd3001_inoi delay after reset and final DBI timing. */
	{ 0x00, 0, 50, {} },
	{ 0xfd, 2, 0, { 0x06, 0x08 } },
	{ 0x61, 2, 0, { 0x07, 0x07 } },
	{ 0x73, 1, 0, { 0x70 } },
	{ 0x73, 1, 0, { 0x00 } },
	{ 0x62, 3, 0, { 0x00, 0x44, 0x40 } },
	{ 0x63, 4, 0, { 0x41, 0x07, 0x12, 0x12 } },
	{ 0x65, 3, 0, { 0x09, 0x10, 0x21 } },
	{ 0x66, 3, 0, { 0x09, 0x10, 0x21 } },
	{ 0x67, 2, 0, { 0x21, 0x40 } },
	{ 0x68, 4, 0, { 0xa5, 0x4c, 0x45, 0x21 } },
	{ 0xb1, 3, 0, { 0x0f, 0x02, 0x02 } },
	{ 0xb4, 1, 0, { 0x01 } },
	{ 0xb5, 4, 0, { 0x02, 0x02, 0x0a, 0x14 } },
	{ 0xb6, 5, 0, { 0x04, 0x00, 0x9f, 0x00, 0x02 } },
	{ 0xdf, 1, 0, { 0x11 } },
	{ 0xe2, 6, 0, { 0x1a, 0x14, 0x14, 0x1e, 0x1b, 0x3f } },
	{ 0xe5, 6, 0, { 0x3f, 0x1c, 0x1d, 0x12, 0x13, 0x1a } },
	{ 0xe1, 2, 0, { 0x39, 0x6c } },
	{ 0xe4, 2, 0, { 0x69, 0x32 } },
	{ 0xe0, 8, 0, { 0x08, 0x0d, 0x10, 0x12, 0x14, 0x10, 0x11, 0x14 } },
	{ 0xe3, 8, 0, { 0x16, 0x13, 0x15, 0x16, 0x15, 0x12, 0x0c, 0x08 } },
	{ 0xe6, 2, 0, { 0x00, 0xff } },
	{ 0xe7, 6, 0, { 0x01, 0x04, 0x03, 0x03, 0x00, 0x12 } },
	{ 0xe8, 3, 0, { 0x00, 0x70, 0x00 } },
	{ 0xec, 1, 0, { 0x52 } },
	{ 0xf6, 4, 0, { 0x01, 0x30, 0x00, 0x00 } },
	{ 0xf1, 3, 0, { 0x01, 0x61, 0x62 } },
	{ 0xfd, 2, 0, { 0xfa, 0xfc } },
	{ 0x3a, 1, 0, { 0x55 } },
	{ 0x35, 1, 0, { 0x00 } },
	{ 0x11, 0, 200, {} },
	{ 0x29, 0, 0, {} },
	{ 0x36, 1, 0, { 0x00 } },
	{ 0x2a, 4, 0, { 0x00, 0x00, 0x00, 0xef } },
	{ 0x2b, 4, 0, { 0x00, 0x00, 0x01, 0x3f } },
};

static const struct ums9117_fb_profile inoi244_fb_profile = {
	.name = "inoi244-rgb565",
	.transport = UMS9117_FB_TRANSPORT_LCM_DBI,
	.completion = UMS9117_FB_COMPLETION_POLL,
	.init = inoi244_panel_init,
	.init_count = ARRAY_SIZE(inoi244_panel_init),
	.width = 240,
	.height = 320,
	.reset_phase_ms = 10,
	.reset_release_ms = 0,
	.sleep_in_ms = 5,
	.sleep_out_ms = 200,
	.lcdc_ctrl_set = BIT(1),
	.lcdc_ctrl_clear = BIT(2) | (7U << 5),
};

static int inoi244_fb_probe(struct platform_device *pdev)
{
	return ums9117_fb_probe(pdev, &inoi244_fb_profile);
}

static const struct of_device_id inoi244_fb_of_match[] = {
	{ .compatible = "fplinux,ums9117-nv3030-lcm-fb" },
	{}
};
MODULE_DEVICE_TABLE(of, inoi244_fb_of_match);

static struct platform_driver inoi244_fb_driver = {
	.probe = inoi244_fb_probe,
	.remove = ums9117_fb_remove,
	.shutdown = ums9117_fb_shutdown,
	.driver = {
		.name = "inoi244-fb",
		.of_match_table = inoi244_fb_of_match,
	},
};
module_platform_driver(inoi244_fb_driver);

MODULE_DESCRIPTION("INOI 244 Modern 4G NV3030 panel profile");
MODULE_LICENSE("GPL");
