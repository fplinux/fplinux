/* SPDX-License-Identifier: GPL-2.0-only */
/* Test-owned display and delay boundary; no physical display is exercised. */
#define _DEFAULT_SOURCE
#include "fplinux-drm-session.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool fplinux_drm_session_open(struct fplinux_drm_session *session,
			      const char *drm, const char *tty, uint32_t format,
			      char *error, size_t size)
{
	static uint16_t pixels[6];
	(void)drm;
	(void)format;
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
	puts("stub display opened");
	return true;
}

bool fplinux_drm_session_present(struct fplinux_drm_session *session,
				 unsigned int page)
{
	(void)session;
	(void)page;
	puts("stub preview presented");
	return true;
}

bool fplinux_drm_session_close(struct fplinux_drm_session *session)
{
	(void)session;
	return true;
}

/* A fixed clock makes the application's requested hold duration observable. */
int clock_gettime(clockid_t clock_id, struct timespec *value)
{
	(void)clock_id;
	value->tv_sec = 1;
	value->tv_nsec = 0;
	return 0;
}

bool fplinux_drm_session_wait_until(struct fplinux_drm_session *session,
				    const struct timespec *deadline)
{
	uint64_t microseconds = (uint64_t)(deadline->tv_sec - 1) * 1000000U +
				deadline->tv_nsec / 1000U;

	(void)session;
	printf("stub hold_us=%llu\n", (unsigned long long)microseconds);
	return true;
}
