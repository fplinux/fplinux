/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_DRM_SESSION_H
#define FPLINUX_DRM_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

struct fplinux_drm_session;
typedef bool (*fplinux_drm_active_handler)(struct fplinux_drm_session *session,
					   bool active, void *data);

struct fplinux_drm_property_ids {
	uint32_t connector_crtc_id;
	uint32_t crtc_mode_id;
	uint32_t crtc_active;
	uint32_t crtc_out_fence_ptr;
	uint32_t plane_fb_id;
	uint32_t plane_crtc_id;
	uint32_t plane_crtc_x;
	uint32_t plane_crtc_y;
	uint32_t plane_crtc_w;
	uint32_t plane_crtc_h;
	uint32_t plane_src_x;
	uint32_t plane_src_y;
	uint32_t plane_src_w;
	uint32_t plane_src_h;
};

struct fplinux_drm_session {
	uint8_t *mapping;
	size_t size;
	size_t page_bytes;
	size_t stride;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	unsigned int pages;
	unsigned int shown_page;
	unsigned int pending_page;
	bool active;
	bool presented;
	bool buffer_acquired;
	int vt;
	int previous_vt;
	int drm;
	int tty;
	int control;
	int signal_fd;
	int guardian_fd;
	int present_fence;
	int presentation_error;
	pid_t guardian;
	bool signals_blocked;
	bool vt_owned;
	sigset_t previous_mask;
	uint32_t connector;
	uint32_t crtc;
	uint32_t plane;
	uint32_t mode_blob;
	uint32_t handle;
	uint32_t framebuffers[2];
	struct fplinux_drm_property_ids property_ids;
	drmModeModeInfo mode;
	fplinux_drm_active_handler active_handler;
	void *active_data;
};

/*
 * NULL drm_path selects the unique connected display supporting format.
 * The process owns one VT; children must exec before acquiring a new session.
 */
bool fplinux_drm_session_open(struct fplinux_drm_session *session,
			      const char *drm_path, const char *tty_path,
			      uint32_t format, char *error, size_t error_size);
bool fplinux_drm_session_set_active_handler(struct fplinux_drm_session *session,
					    fplinux_drm_active_handler handler,
					    void *data);
int fplinux_drm_session_fd(const struct fplinux_drm_session *session);
bool fplinux_drm_session_dispatch(struct fplinux_drm_session *session);
/* Preserve VT responsiveness while holding a frame or pacing rendering. */
bool fplinux_drm_session_wait_until(struct fplinux_drm_session *session,
				    const struct timespec *deadline);
/* Completion includes the fence's operation status, not only its signal. */
bool fplinux_drm_session_present(struct fplinux_drm_session *session,
				 unsigned int page);
/* Queue one frame; EBUSY leaves the previous frame pending. */
bool fplinux_drm_session_queue_present(struct fplinux_drm_session *session,
				       unsigned int page);
/*
 * Finish a queued frame before writing any part of the shared display buffer.
 * A completion error terminates presentation until the session is closed.
 */
bool fplinux_drm_session_wait_present(struct fplinux_drm_session *session);
/* Close the owned exported fd before releasing the exclusive buffer borrow. */
bool fplinux_drm_session_acquire_buffer(struct fplinux_drm_session *session,
					int *fd);
/* Drains pending access; completion failures remain visible through wait/close. */
void fplinux_drm_session_release_buffer(struct fplinux_drm_session *session);
bool fplinux_drm_session_close(struct fplinux_drm_session *session);

#endif
