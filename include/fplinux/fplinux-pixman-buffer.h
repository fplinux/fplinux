/* SPDX-License-Identifier: MIT */
#ifndef FPLINUX_PIXMAN_BUFFER_H
#define FPLINUX_PIXMAN_BUFFER_H

/* Private shared contract between the phone shell and its pinned Weston. */
#include <pixman.h>

struct weston_compositor;
struct weston_surface;
struct weston_buffer_reference;

/* Takes an image reference. The caller owns the memory behind that image. */
struct weston_buffer_reference *
weston_buffer_create_internal_image(struct weston_compositor *compositor,
				    pixman_image_t *image);
void weston_surface_attach_internal_image(
	struct weston_surface *surface,
	struct weston_buffer_reference *buffer_ref);
/* Destroy the attached surface first, then release the owner's reference. */
void weston_buffer_destroy_internal_image(
	struct weston_buffer_reference *buffer_ref);

#endif
