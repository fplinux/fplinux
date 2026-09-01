/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * UMS9117 fbdev core contract.
 *
 * A board wrapper owns the panel profile and calls these helpers from its
 * platform_driver callbacks.  The core owns the framebuffer, LCDC lifecycle,
 * damage coalescing, completion IRQ, WLED and the fail-dark path; the common
 * SPI and LCM files own only their respective wire protocols.
 */
#ifndef FPLINUX_UMS9117_FB_H
#define FPLINUX_UMS9117_FB_H

#include <linux/pm.h>
#include <linux/types.h>

struct platform_device;

enum ums9117_fb_transport_kind {
	UMS9117_FB_TRANSPORT_SPI1_3WIRE,
	UMS9117_FB_TRANSPORT_LCM_DBI,
};

/* Completion delivery is a fixed property of the board profile. */
enum ums9117_fb_completion_kind {
	UMS9117_FB_COMPLETION_IRQ,
	UMS9117_FB_COMPLETION_POLL,
};

/*
 * A DCS command followed by zero or more byte parameters and an optional
 * delay. command == 0 with length == 0 is a delay-only script step.
 */
struct ums9117_fb_command {
	u8 command;
	u8 length;
	u16 delay_ms;
	u8 data[16];
};

/*
 * This is deliberately board-owned.  It contains only panel facts verified
 * for one target, never DT-supplied command bytes or transport heuristics.
 */
struct ums9117_fb_profile {
	const char *name;
	enum ums9117_fb_transport_kind transport;
	enum ums9117_fb_completion_kind completion;
	const struct ums9117_fb_command *init;
	unsigned int init_count;
	u16 width;
	u16 height;
	u16 reset_phase_ms;
	u16 reset_release_ms;
	u16 sleep_in_ms;
	u16 sleep_out_ms;
	/*
	 * NULL retains the fixed WLED recipe. A name enables a raw range whose
	 * maximum is derived from equal non-zero board WLED current levels.
	 */
	const char *wled_backlight_name;
	/* LCDC CTRL bits required by this panel's transport. */
	u32 lcdc_ctrl_set;
	u32 lcdc_ctrl_clear;
};

int ums9117_fb_probe(struct platform_device *pdev,
		     const struct ums9117_fb_profile *profile);
void ums9117_fb_remove(struct platform_device *pdev);
void ums9117_fb_shutdown(struct platform_device *pdev);
extern const struct dev_pm_ops ums9117_fb_pm_ops;

#endif /* FPLINUX_UMS9117_FB_H */
