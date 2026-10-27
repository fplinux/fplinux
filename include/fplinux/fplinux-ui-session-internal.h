/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_SESSION_INTERNAL_H
#define FPLINUX_UI_SESSION_INTERNAL_H

#include "fplinux-ui-session.h"

/* Protocol transport linked by the runtime, session manager and host peers. */
bool fplinux_ui_session_vibrate_valid(
	const struct fplinux_ui_session_message *message);

/* Checks a local regular file without following any path-component symlink. */
int fplinux_ui_session_document_path_error(const char *path);

/* connect sends HELLO and returns a nonblocking, close-on-exec descriptor. */
int fplinux_ui_session_connect(const char *app_id);
int fplinux_ui_session_connect_at(const char *path, const char *app_id);
bool fplinux_ui_session_send(int fd,
			     const struct fplinux_ui_session_message *message);
/*
 * Returns 1 for a packet, 0 for EAGAIN, -1 for a disconnect or invalid packet.
 * A successful packet with has_frame transfers frame_fd ownership to the
 * caller. Rejected packets close every attached descriptor internally.
 */
int fplinux_ui_session_receive(int fd,
			       struct fplinux_ui_session_message *message);

#endif
