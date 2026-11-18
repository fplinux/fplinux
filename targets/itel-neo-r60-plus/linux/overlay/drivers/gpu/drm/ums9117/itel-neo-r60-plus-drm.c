// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ums9117-drm.h"
#include "fplinux-itel-neo-r60-plus-panel.h"

static const struct ums9117_drm_profile itel_neo_r60_plus_drm_profile = {
	.name = "itel-neo-r60-plus-rgb565",
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
	.display_off_ms = 10,
	.sleep_in_ms = 120,
	.spi_frame_clock_divider = 1,
	.spi_init_clock_divider = 1,
	.spi_mode = UMS9117_DRM_SPI_DUAL_DATA,
	.wled_backlight_name = "itel-neo-r60-plus-backlight",
	.wled_duty = 0xffff,
	.lcdc_ctrl_set = BIT(2) | (7U << 5),
	.lcdc_ctrl_clear = BIT(1) | BIT(4),
};

static const struct of_device_id itel_neo_r60_plus_drm_of_match[] = {
	{ .compatible = "fplinux,ums9117-st7789h2-spi-fb",
	  .data = &itel_neo_r60_plus_drm_profile },
	{}
};
MODULE_DEVICE_TABLE(of, itel_neo_r60_plus_drm_of_match);

static struct platform_driver itel_neo_r60_plus_drm_driver = {
	.probe = ums9117_drm_probe,
	.remove = ums9117_drm_remove,
	.shutdown = ums9117_drm_shutdown,
	.driver = {
		.name = "itel-neo-r60-plus-drm",
		.of_match_table = itel_neo_r60_plus_drm_of_match,
		.pm = pm_sleep_ptr(&ums9117_drm_pm_ops),
	},
};
module_platform_driver(itel_neo_r60_plus_drm_driver);

MODULE_DESCRIPTION("itel Neo R60+ SPI panel profile");
MODULE_LICENSE("GPL");
