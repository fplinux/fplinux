/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-drm-session.h"
#include "fplinux-font.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned int frames;
static unsigned int text_size;
static char text[1024];

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode)
{
	if (!strcmp(path, "/usr/share/fplinux/fonts/ter-u12n.psf") ||
	    !strcmp(path, "/usr/share/fplinux/fonts/ter-u16n.psf"))
		path = getenv("FPLINUX_TEST_FONT");
	return __real_fopen(path, mode);
}

const unsigned char *__real_fplinux_font_glyph(const struct fplinux_font *font,
					       uint32_t codepoint);
const unsigned char *__wrap_fplinux_font_glyph(const struct fplinux_font *font,
					       uint32_t codepoint)
{
	assert(text_size + 1 < sizeof(text));
	text[text_size++] = (char)codepoint;
	text[text_size] = '\0';
	return __real_fplinux_font_glyph(font, codepoint);
}

bool fplinux_drm_session_open(struct fplinux_drm_session *session,
			      const char *drm_path, const char *tty_path,
			      uint32_t format, char *error, size_t error_size)
{
	const char *geometry = getenv("FPLINUX_TEST_GEOMETRY");
	const char *events = getenv("FPLINUX_TEST_VT");

	(void)error;
	(void)error_size;
	assert(!drm_path && !tty_path && format == DRM_FORMAT_RGB565);
	assert(geometry && events);
	memset(session, 0, sizeof(*session));
	assert(sscanf(geometry, "%ux%u", &session->width, &session->height) ==
	       2);
	session->stride = (size_t)session->width * 2 + 8;
	session->page_bytes = session->stride * session->height;
	session->pages = 2;
	session->size = session->page_bytes * session->pages;
	session->mapping = malloc(session->size + 32);
	assert(session->mapping);
	memset(session->mapping, 0xa5, session->size + 32);
	session->signal_fd = open(events, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	assert(session->signal_fd >= 0);
	session->active = true;
	puts("opened");
	fflush(stdout);
	return true;
}

bool fplinux_drm_session_set_active_handler(struct fplinux_drm_session *session,
					    fplinux_drm_active_handler handler,
					    void *data)
{
	session->active_handler = handler;
	session->active_data = data;
	return !handler || handler(session, session->active, data);
}

int fplinux_drm_session_fd(const struct fplinux_drm_session *session)
{
	return session->signal_fd;
}

bool fplinux_drm_session_dispatch(struct fplinux_drm_session *session)
{
	char active;

	assert(read(session->signal_fd, &active, 1) == 1);
	session->active = active == '1';
	assert(session->active_handler(session, session->active,
				       session->active_data));
	puts(session->active ? "active" : "inactive");
	fflush(stdout);
	return true;
}

bool fplinux_drm_session_wait_present(struct fplinux_drm_session *session)
{
	(void)session;
	return true;
}

bool fplinux_drm_session_present(struct fplinux_drm_session *session,
				 unsigned int page)
{
	FILE *frame = __real_fopen(getenv("FPLINUX_TEST_FRAME"), "wb");
	unsigned char *pixels = session->mapping + page * session->page_bytes;

	assert(frame && session->active && page < session->pages);
	for (unsigned int row = 0; row < session->height; ++row) {
		assert(fwrite(pixels + row * session->stride, 2, session->width,
			      frame) == session->width);
		for (unsigned int byte = session->width * 2;
		     byte < session->stride; ++byte)
			assert(pixels[row * session->stride + byte] == 0xa5);
	}
	assert(!fclose(frame));
	for (size_t byte = session->size; byte < session->size + 32; ++byte)
		assert(session->mapping[byte] == 0xa5);
	session->shown_page = page;
	printf("frame %u %s\n", ++frames, text);
	text_size = 0;
	text[0] = '\0';
	fflush(stdout);
	return true;
}

bool fplinux_drm_session_close(struct fplinux_drm_session *session)
{
	assert(!session->active_handler);
	close(session->signal_fd);
	free(session->mapping);
	puts("closed");
	fflush(stdout);
	return true;
}
