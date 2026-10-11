/* SPDX-License-Identifier: GPL-2.0-only */
/* Fake display activation boundary; no VT or DRM device is opened. */
#ifndef FPLINUX_TEST_DRM_SESSION_H
#define FPLINUX_TEST_DRM_SESSION_H

#include <stdbool.h>
#include <stdint.h>

struct fplinux_drm_session {
	uint32_t width;
	uint32_t height;
	bool active;
};
typedef bool (*fplinux_drm_active_handler)(struct fplinux_drm_session *session,
					   bool active, void *data);
bool fplinux_drm_session_set_active_handler(struct fplinux_drm_session *session,
					    fplinux_drm_active_handler handler,
					    void *data);

#endif
