/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-fb-session.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void set_error(char *error, size_t size, const char *message)
{
	if (size)
		snprintf(error, size, "%s: %s", message, strerror(errno));
}

static void set_message(char *error, size_t size, const char *message)
{
	if (size)
		snprintf(error, size, "%s", message);
}

bool fplinux_fb_session_layout_valid(const struct fb_fix_screeninfo *fixed,
				     const struct fb_var_screeninfo *variable,
				     size_t *page_bytes, unsigned int *pages)
{
	size_t bytes;
	unsigned int count;

	if (!fixed || !variable || fixed->type != FB_TYPE_PACKED_PIXELS ||
	    fixed->visual != FB_VISUAL_TRUECOLOR || variable->xres == 0 ||
	    variable->yres == 0 || variable->xres_virtual != variable->xres ||
	    variable->xoffset != 0 || variable->bits_per_pixel != 16 ||
	    variable->red.offset != 11 || variable->red.length != 5 ||
	    variable->red.msb_right != 0 || variable->green.offset != 5 ||
	    variable->green.length != 6 || variable->green.msb_right != 0 ||
	    variable->blue.offset != 0 || variable->blue.length != 5 ||
	    variable->blue.msb_right != 0 || variable->transp.length != 0 ||
	    variable->xres > UINT32_MAX / sizeof(uint16_t) ||
	    fixed->line_length < variable->xres * sizeof(uint16_t) ||
	    fixed->line_length > UINT32_MAX / variable->yres)
		return false;
	if (variable->yres_virtual != variable->yres &&
	    (variable->yres > UINT32_MAX / 2U ||
	     variable->yres_virtual != variable->yres * 2U))
		return false;
	if ((variable->yoffset != 0 && variable->yoffset != variable->yres) ||
	    variable->yoffset + variable->yres > variable->yres_virtual)
		return false;
	count = variable->yres_virtual == variable->yres * 2U ? 2U : 1U;
	bytes = (size_t)fixed->line_length * variable->yres;
	if (fixed->smem_len < bytes ||
	    (count == 2U && (fixed->ypanstep == 0 || bytes > UINT32_MAX / 2U ||
			     fixed->smem_len < bytes * 2U)))
		return false;
	if (page_bytes)
		*page_bytes = bytes;
	if (pages)
		*pages = count;
	return true;
}

static void release_session(struct fplinux_fb_session *session)
{
	free(session->backup);
	if (session->mapping)
		munmap(session->mapping, session->size);
	if (session->framebuffer >= 0)
		close(session->framebuffer);
	if (session->tty >= 0)
		close(session->tty);
	memset(session, 0, sizeof(*session));
	session->framebuffer = -1;
	session->tty = -1;
}

bool fplinux_fb_session_open(struct fplinux_fb_session *session,
			     const char *framebuffer_path, const char *tty_path,
			     char *error, size_t error_size)
{
	struct fb_fix_screeninfo fixed;
	struct vt_stat vt;

	if (!session || !framebuffer_path || !tty_path) {
		errno = EINVAL;
		set_error(error, error_size,
			  "invalid framebuffer session request");
		return false;
	}
	memset(session, 0, sizeof(*session));
	session->framebuffer = -1;
	session->tty = -1;
	session->tty = open(tty_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (session->tty < 0) {
		set_error(error, error_size, "cannot open console");
		goto fail;
	}
	if (ioctl(session->tty, KDGETMODE, &session->tty_mode) < 0 ||
	    ioctl(session->tty, VT_GETSTATE, &vt) < 0) {
		set_error(error, error_size, "cannot read console state");
		goto fail;
	}
	if (session->tty_mode != KD_TEXT) {
		errno = EBUSY;
		set_message(error, error_size,
			    "active console is not in text mode");
		goto fail;
	}
	session->active_vt = vt.v_active;
	session->framebuffer = open(framebuffer_path, O_RDWR | O_CLOEXEC);
	if (session->framebuffer < 0) {
		set_error(error, error_size, "cannot open framebuffer");
		goto fail;
	}
	if (ioctl(session->framebuffer, FBIOGET_FSCREENINFO, &fixed) < 0 ||
	    ioctl(session->framebuffer, FBIOGET_VSCREENINFO,
		  &session->variable) < 0) {
		set_error(error, error_size, "cannot read framebuffer state");
		goto fail;
	}
	if (!fplinux_fb_session_layout_valid(&fixed, &session->variable,
					     &session->page_bytes,
					     &session->pages)) {
		errno = EINVAL;
		set_message(error, error_size, "unexpected framebuffer ABI");
		goto fail;
	}
	session->size = fixed.smem_len;
	session->stride = fixed.line_length;
	session->width = session->variable.xres;
	session->height = session->variable.yres;
	session->shown_page = session->variable.yoffset / session->height;
	session->mapping = mmap(NULL, session->size, PROT_READ | PROT_WRITE,
				MAP_SHARED, session->framebuffer, 0);
	if (session->mapping == MAP_FAILED) {
		session->mapping = NULL;
		set_error(error, error_size, "cannot map framebuffer");
		goto fail;
	}
	session->backup = malloc(session->size);
	if (!session->backup) {
		set_error(error, error_size,
			  "cannot allocate framebuffer backup");
		goto fail;
	}
	memcpy(session->backup, session->mapping, session->size);
	return true;

fail:
	release_session(session);
	return false;
}

bool fplinux_fb_session_set_graphics(struct fplinux_fb_session *session,
				     char *error, size_t error_size)
{
	if (!session || session->tty < 0) {
		errno = EINVAL;
		set_error(error, error_size, "invalid framebuffer session");
		return false;
	}
	if (ioctl(session->tty, KDSETMODE, KD_GRAPHICS) < 0) {
		set_error(error, error_size,
			  "cannot set console graphics mode");
		return false;
	}
	return true;
}

bool fplinux_fb_session_present(struct fplinux_fb_session *session,
				unsigned int page)
{
	struct fb_var_screeninfo variable;

	if (!session || session->framebuffer < 0 || page >= session->pages) {
		errno = EINVAL;
		return false;
	}
	if (session->pages == 1U) {
		session->shown_page = 0;
		return true;
	}
	variable = session->variable;
	variable.xoffset = 0;
	variable.yoffset = page * variable.yres;
	variable.activate = FB_ACTIVATE_NOW;
	if (ioctl(session->framebuffer, FBIOPAN_DISPLAY, &variable) < 0)
		return false;
	session->shown_page = page;
	return true;
}

bool fplinux_fb_session_close(struct fplinux_fb_session *session)
{
	struct fb_var_screeninfo variable;
	bool ok = true;

	if (!session)
		return false;
	variable = session->variable;
	if (session->mapping && session->backup) {
		memcpy(session->mapping, session->backup, session->size);
		__sync_synchronize();
	}
	if (session->framebuffer >= 0) {
		variable.activate = FB_ACTIVATE_NOW;
		if (ioctl(session->framebuffer, FBIOPUT_VSCREENINFO,
			  &variable) < 0)
			ok = false;
		if (session->pages == 2U &&
		    ioctl(session->framebuffer, FBIOPAN_DISPLAY, &variable) < 0)
			ok = false;
	}
	if (session->tty >= 0) {
		if (ioctl(session->tty, KDSETMODE, session->tty_mode) < 0)
			ok = false;
		if (ioctl(session->tty, VT_ACTIVATE, session->active_vt) < 0)
			ok = false;
		if (ioctl(session->tty, VT_WAITACTIVE, session->active_vt) < 0)
			ok = false;
	}
	if (session->mapping && munmap(session->mapping, session->size) < 0)
		ok = false;
	free(session->backup);
	if (session->framebuffer >= 0 && close(session->framebuffer) < 0)
		ok = false;
	if (session->tty >= 0 && close(session->tty) < 0)
		ok = false;
	memset(session, 0, sizeof(*session));
	session->framebuffer = -1;
	session->tty = -1;
	return ok;
}
