/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_DRM_INTERNAL_H
#define FPLINUX_UMS9117_DRM_INTERNAL_H

#include <linux/backlight.h>
#include <linux/completion.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_simple_kms_helper.h>

#include "ums9117-drm.h"

#define UMS9117_DRM_WLED_CHANNEL_COUNT 4

enum ums9117_drm_panel_state {
	UMS9117_DRM_PANEL_STATE_COLD_INIT,
	UMS9117_DRM_PANEL_STATE_ACTIVE,
	UMS9117_DRM_PANEL_STATE_BLANKING,
	UMS9117_DRM_PANEL_STATE_BLANKED,
	UMS9117_DRM_PANEL_STATE_WAKING,
	UMS9117_DRM_PANEL_STATE_ERROR,
};

struct ums9117_drm_stats {
	u64 frames_started;
	u64 frames_done_irq;
	u64 frame_timeouts;
	u64 irq_spurious;
	u64 irq_missed;
	u64 blank_count;
	u64 blank_completed;
	u64 wake_count;
	u64 dcs_errors;
	u64 dcs_timeouts;
	u64 wled_errors;
	u64 fail_dark_failures;
	u64 present_nv16;
	u64 present_rgb565;
	u32 last_error_irq_status;
	u32 last_error_irq_raw;
};

struct ums9117_drm {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	struct backlight_device *backlight;
	const struct ums9117_drm_profile *profile;
	void __iomem *lcdc;
	void __iomem *lcm;
	void __iomem *ap_ahb_gate_set;
	void __iomem *ap_ahb_reset_set;
	void __iomem *ap_ahb_reset_clear;
	void __iomem *spi;
	void __iomem *spi_clock_selector;
	void __iomem *spi_reset_set;
	void __iomem *spi_reset_clear;
	void __iomem *lcm_command;
	void __iomem *lcm_data;
	phys_addr_t stream_phys;
	struct regmap *aon_apb;
	struct regmap *pmic;
	u32 lcm_timing;
	struct completion frame_done;
	/* Serializes complete transfers, panel transitions and WLED updates. */
	struct mutex panel_lock;
	spinlock_t lock;
	struct ums9117_drm_stats stats;
	u32 wled_levels[UMS9117_DRM_WLED_CHANNEL_COUNT];
	u32 backlight_max_brightness;
	u32 backlight_effective;
	u32 backlight_applied;
	u32 rgb_img_ctrl;
	u32 rgb_y2r_ctrl;
	u32 rgb_y2r_contrast;
	u32 rgb_y2r_saturation;
	u32 rgb_y2r_brightness;
	u64 last_transfer_ns;
	dma_addr_t last_y_address;
	dma_addr_t last_uv_address;
	int irq;
	int last_error_errno;
	u8 last_dcs_command;
	enum ums9117_drm_panel_state state;
	bool stopping;
	bool in_flight;
	bool transport_faulted;
	bool wled_known;
	bool wled_on;
};

int ums9117_drm_spi_init_transport(struct ums9117_drm *udrm,
				   struct platform_device *pdev);
int ums9117_drm_spi_enable_transport(struct ums9117_drm *udrm);
int ums9117_drm_spi_post_reset(struct ums9117_drm *udrm);
int ums9117_drm_spi_dcs(struct ums9117_drm *udrm, u8 command, const u8 *data,
			size_t length);
int ums9117_drm_spi_begin_frame(struct ums9117_drm *udrm);
int ums9117_drm_lcm_init_transport(struct ums9117_drm *udrm,
				   struct platform_device *pdev);
int ums9117_drm_lcm_enable_transport(struct ums9117_drm *udrm);
int ums9117_drm_lcm_post_reset(struct ums9117_drm *udrm);
int ums9117_drm_lcm_dcs(struct ums9117_drm *udrm, u8 command, const u8 *data,
			size_t length);
int ums9117_drm_lcm_begin_frame(struct ums9117_drm *udrm);

#endif /* FPLINUX_UMS9117_DRM_INTERNAL_H */
