/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_FB_INTERNAL_H
#define FPLINUX_UMS9117_FB_INTERNAL_H

#include <linux/completion.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <linux/soc/sprd/ums9117-adi.h>

#include "ums9117-fb.h"

#define UMS9117_FB_WLED_CHANNEL_COUNT 4

enum ums9117_fb_panel_state {
	UMS9117_FB_PANEL_STATE_COLD_INIT,
	UMS9117_FB_PANEL_STATE_ACTIVE,
	UMS9117_FB_PANEL_STATE_BLANKING,
	UMS9117_FB_PANEL_STATE_BLANKED,
	UMS9117_FB_PANEL_STATE_WAKING,
	UMS9117_FB_PANEL_STATE_ERROR,
};

struct ums9117_fb_stats {
	u64 frames_started;
	u64 frames_done_irq;
	u64 frames_done_poll;
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
	u32 last_error_irq_status;
	u32 last_error_irq_raw;
};

struct ums9117_fb {
	struct fb_info *info;
	const struct ums9117_fb_profile *profile;
	void __iomem *screen;
	void __iomem *transfer;
	void *snapshot;
	void __iomem *lcdc;
	void __iomem *lcm;
	void __iomem *ap_ahb_gate_set;
	void __iomem *ap_ahb_reset_set;
	void __iomem *ap_ahb_reset_clear;
	void __iomem *pinmux;
	void __iomem *pinconf;
	/* Transport-private MMIO is initialized by ums9117-fb-spi/lcm.c. */
	void __iomem *spi;
	void __iomem *spi_clock_selector;
	void __iomem *spi_reset_set;
	void __iomem *spi_reset_clear;
	void __iomem *lcm_command;
	void __iomem *lcm_data;
	phys_addr_t screen_phys;
	phys_addr_t transfer_phys;
	phys_addr_t stream_phys;
	struct regmap *aon_apb;
	struct ums9117_adi_transaction adi_transaction;
	u32 lcm_timing;
	struct work_struct refresh_work;
	struct work_struct wake_work;
	struct work_struct poll_work;
	struct delayed_work timeout_work;
	struct completion frame_done;
	struct mutex transition_lock;
	struct mutex panel_lock;
	spinlock_t lock;
	struct ums9117_fb_stats stats;
	u32 pseudo_palette[16];
	u32 pinmux_count;
	u32 pinconf_count;
	u32 wled_levels[UMS9117_FB_WLED_CHANNEL_COUNT];
	unsigned long frame_deadline;
	u64 damage_seq;
	u64 submitted_seq;
	u64 generation;
	u64 done_generation;
	unsigned int shown;
	int irq;
	int last_error_errno;
	u8 last_dcs_command;
	enum ums9117_fb_panel_state state;
	bool stopping;
	bool in_flight;
	bool transport_faulted;
	bool wled_known;
	bool wled_on;
	bool audit_file_created;
	bool pm_restore_active;
};

int ums9117_fb_transport_init(struct ums9117_fb *ufb,
			      struct platform_device *pdev);
int ums9117_fb_transport_enable(struct ums9117_fb *ufb);
int ums9117_fb_transport_post_reset(struct ums9117_fb *ufb);
int ums9117_fb_spi_init_transport(struct ums9117_fb *ufb,
				  struct platform_device *pdev);
int ums9117_fb_spi_enable_transport(struct ums9117_fb *ufb);
int ums9117_fb_spi_post_reset(struct ums9117_fb *ufb);
int ums9117_fb_spi_dcs(struct ums9117_fb *ufb, u8 command, const u8 *data,
		       size_t length);
int ums9117_fb_spi_begin_frame(struct ums9117_fb *ufb);
int ums9117_fb_lcm_init_transport(struct ums9117_fb *ufb,
				  struct platform_device *pdev);
int ums9117_fb_lcm_enable_transport(struct ums9117_fb *ufb);
int ums9117_fb_lcm_post_reset(struct ums9117_fb *ufb);
int ums9117_fb_lcm_dcs(struct ums9117_fb *ufb, u8 command, const u8 *data,
		       size_t length);
int ums9117_fb_lcm_begin_frame(struct ums9117_fb *ufb);
u32 ums9117_fb_lcm_dbi_timing_for_test(const u32 ns[6]);

#endif /* FPLINUX_UMS9117_FB_INTERNAL_H */
