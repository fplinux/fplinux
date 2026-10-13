// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"

#include "fplinux-bq-2830l-tank-connect-max-panel.h"

static const struct ums9117_drm_profile bq2830l_drm_profile = {
	.name = "bq-2830l-rgb565",
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
