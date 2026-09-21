/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FB_SESSION_H
#define FPLINUX_FB_SESSION_H

#include <linux/fb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fplinux_fb_session {
	int framebuffer;
	int tty;
	int tty_mode;
	int active_vt;
	uint8_t *mapping;
	uint8_t *backup;
	size_t size;
	size_t page_bytes;
	size_t stride;
	uint32_t width;
	uint32_t height;
	unsigned int pages;
	unsigned int shown_page;
	struct fb_var_screeninfo variable;
};

bool fplinux_fb_session_layout_valid(const struct fb_fix_screeninfo *fixed,
				     const struct fb_var_screeninfo *variable,
				     size_t *page_bytes, unsigned int *pages);
bool fplinux_fb_session_open(struct fplinux_fb_session *session,
			     const char *framebuffer_path, const char *tty_path,
			     char *error, size_t error_size);
bool fplinux_fb_session_set_graphics(struct fplinux_fb_session *session,
				     char *error, size_t error_size);
bool fplinux_fb_session_present(struct fplinux_fb_session *session,
				unsigned int page);
bool fplinux_fb_session_close(struct fplinux_fb_session *session);

#endif
