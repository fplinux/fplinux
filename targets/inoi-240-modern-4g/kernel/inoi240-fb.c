// SPDX-License-Identifier: GPL-2.0-only
/* INOI 240 Modern 4G NV3023 profile for the common UMS9117 fbdev core. */
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-fb.h"

/* Exact LCM/DBI sequence from fpdoom model 103 (commit 9695f3739639). */
static const struct ums9117_fb_command inoi240_panel_init[] = {
	/* Exact cmd3025_inoi delay after reset and final DBI timing. */
	{ 0x00, 0, 10, {} },
	{ 0xff, 1, 0, { 0xa5 } },
	{ 0x3e, 1, 0, { 0x08 } },
	{ 0x3a, 1, 0, { 0x65 } },
	{ 0x82, 1, 0, { 0x00 } },
	{ 0x98, 1, 0, { 0x00 } },
	{ 0x63, 1, 0, { 0x0f } },
	{ 0x64, 1, 0, { 0x0f } },
	{ 0xb4, 1, 0, { 0x54 } },
	{ 0xb5, 1, 0, { 0x30 } },
	{ 0x83, 1, 0, { 0x03 } },
	{ 0x86, 1, 0, { 0x04 } },
	{ 0x87, 1, 0, { 0x16 } },
	{ 0x88, 1, 0, { 0x09 } },
	{ 0x89, 1, 0, { 0x2f } },
	{ 0x93, 1, 0, { 0x63 } },
	{ 0x96, 1, 0, { 0x81 } },
	{ 0xc3, 1, 0, { 0x11 } },
	{ 0xe6, 1, 0, { 0x00 } },
	{ 0x99, 1, 0, { 0x01 } },
	{ 0x70, 1, 0, { 0x07 } },
	{ 0x71, 1, 0, { 0x21 } },
	{ 0x72, 1, 0, { 0x0a } },
	{ 0x73, 1, 0, { 0x10 } },
	{ 0x74, 1, 0, { 0x17 } },
	{ 0x75, 1, 0, { 0x1a } },
	{ 0x76, 1, 0, { 0x3f } },
	{ 0x77, 1, 0, { 0x09 } },
	{ 0x78, 1, 0, { 0x05 } },
	{ 0x79, 1, 0, { 0x3f } },
	{ 0x7a, 1, 0, { 0x05 } },
	{ 0x7b, 1, 0, { 0x0c } },
	{ 0x7c, 1, 0, { 0x12 } },
	{ 0x7d, 1, 0, { 0x0a } },
	{ 0x7e, 1, 0, { 0x0a } },
	{ 0x7f, 1, 0, { 0x08 } },
	{ 0xa0, 1, 0, { 0x0b } },
	{ 0xa1, 1, 0, { 0x30 } },
	{ 0xa2, 1, 0, { 0x09 } },
	{ 0xa3, 1, 0, { 0x0c } },
	{ 0xa4, 1, 0, { 0x08 } },
	{ 0xa5, 1, 0, { 0x22 } },
	{ 0xa6, 1, 0, { 0x40 } },
	{ 0xa7, 1, 0, { 0x04 } },
	{ 0xa8, 1, 0, { 0x05 } },
	{ 0xa9, 1, 0, { 0x3f } },
	{ 0xaa, 1, 0, { 0x0a } },
	{ 0xab, 1, 0, { 0x11 } },
	{ 0xac, 1, 0, { 0x0d } },
	{ 0xad, 1, 0, { 0x06 } },
	{ 0xae, 1, 0, { 0x3b } },
	{ 0xaf, 1, 0, { 0x07 } },
	{ 0xff, 1, 0, { 0x00 } },
	{ 0x11, 0, 200, {} },
	{ 0x35, 1, 0, { 0x00 } },
	{ 0x29, 0, 0, {} },
	{ 0x36, 1, 0, { 0x00 } },
	{ 0x2a, 4, 0, { 0x00, 0x00, 0x00, 0x7f } },
	{ 0x2b, 4, 0, { 0x00, 0x00, 0x00, 0x9f } },
};

static const struct ums9117_fb_profile inoi240_fb_profile = {
	.name = "inoi240-rgb565",
	.transport = UMS9117_FB_TRANSPORT_LCM_DBI,
	.completion = UMS9117_FB_COMPLETION_POLL,
	.init = inoi240_panel_init,
	.init_count = ARRAY_SIZE(inoi240_panel_init),
	.width = 128,
	.height = 160,
	.reset_phase_ms = 10,
	.reset_release_ms = 0,
	.sleep_in_ms = 5,
	.sleep_out_ms = 200,
	.lcdc_ctrl_set = BIT(1),
	.lcdc_ctrl_clear = BIT(2) | (7U << 5),
};

static int inoi240_fb_probe(struct platform_device *pdev)
{
	return ums9117_fb_probe(pdev, &inoi240_fb_profile);
}

static const struct of_device_id inoi240_fb_of_match[] = {
	{ .compatible = "fplinux,ums9117-nv3023-lcm-fb" },
	{}
};
MODULE_DEVICE_TABLE(of, inoi240_fb_of_match);

static struct platform_driver inoi240_fb_driver = {
	.probe = inoi240_fb_probe,
	.remove = ums9117_fb_remove,
	.shutdown = ums9117_fb_shutdown,
	.driver = {
		.name = "inoi240-fb",
		.of_match_table = inoi240_fb_of_match,
	},
};
module_platform_driver(inoi240_fb_driver);

MODULE_DESCRIPTION("INOI 240 Modern 4G NV3023 panel profile");
MODULE_LICENSE("GPL");
