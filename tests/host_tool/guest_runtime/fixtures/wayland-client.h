/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_GUEST_TEST_WAYLAND_CLIENT_H
#define FPLINUX_GUEST_TEST_WAYLAND_CLIENT_H

/* The client API inlines these two requests. Keep the real marshalling. */
#define wl_surface_attach guest_frame_wayland_attach
#define wl_surface_commit guest_frame_wayland_commit
#include_next <wayland-client.h>
#undef wl_surface_attach
#undef wl_surface_commit

void guest_frame_attach(struct wl_surface *surface, struct wl_buffer *buffer,
			int32_t x, int32_t y);
void guest_frame_commit(struct wl_surface *surface);

#define wl_surface_attach guest_frame_attach
#define wl_surface_commit guest_frame_commit

#endif
