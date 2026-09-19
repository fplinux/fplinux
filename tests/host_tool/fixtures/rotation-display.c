/* SPDX-License-Identifier: GPL-2.0-only */
/* Test-owned framebuffer and delay boundary; no physical display is exercised. */
#define _DEFAULT_SOURCE
#include "fplinux-fb-session.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool fplinux_fb_session_open(struct fplinux_fb_session *session,
			     const char *framebuffer, const char *tty,
			     char *error, size_t size)
{
	static uint16_t pixels[6];
	(void)framebuffer;
	(void)tty;
	(void)error;
	(void)size;
	memset(session, 0, sizeof(*session));
	session->mapping = (uint8_t *)pixels;
	session->width = 2;
	session->height = 3;
	session->stride = 4;
	session->pages = 1;
	session->page_bytes = sizeof(pixels);
	puts("stub framebuffer opened");
	return true;
}

bool fplinux_fb_session_set_graphics(struct fplinux_fb_session *session,
				     char *error, size_t size)
{
	(void)session;
	(void)error;
	(void)size;
	return true;
}

bool fplinux_fb_session_present(struct fplinux_fb_session *session,
				unsigned int page)
{
	(void)session;
	(void)page;
	puts("stub preview presented");
	return true;
}

bool fplinux_fb_session_close(struct fplinux_fb_session *session)
{
	(void)session;
	return true;
}

int usleep(useconds_t usec)
{
	printf("stub hold_us=%u\n", usec);
	return 0;
}
