/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <wayland-client.h>

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static bool attached;
static unsigned int commits;

void guest_frame_attach(struct wl_surface *surface, struct wl_buffer *buffer,
			int32_t x, int32_t y)
{
	attached = buffer != NULL;
	guest_frame_wayland_attach(surface, buffer, x, y);
}

void guest_frame_commit(struct wl_surface *surface)
{
	const char *directory = getenv("FPLINUX_APP_COMMIT_DIRECTORY");

	if (directory && attached) {
		const char *record = getenv("FPLINUX_APP_FRAME_RECORD");
		char redraw[PATH_MAX], commit[PATH_MAX];
		int length;

		assert(record);
		length = snprintf(redraw, sizeof(redraw), "%s.layout.json",
				  record);
		assert(length >= 0 && length < (int)sizeof(redraw));
		length = snprintf(commit, sizeof(commit), "%s/%u.json",
				  directory, ++commits);
		assert(length >= 0 && length < (int)sizeof(commit));
		/* Freeze the last completed redraw before this buffer can arrive. */
		assert(link(redraw, commit) == 0);
	}
	attached = false;
	guest_frame_wayland_commit(surface);
}
