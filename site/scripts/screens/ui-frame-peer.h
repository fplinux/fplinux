/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UI_FRAME_PEER_H
#define FPLINUX_UI_FRAME_PEER_H

#include "fplinux-ui-session.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server.h>

struct fplinux_ui_frame_peer;

struct fplinux_ui_frame_keyboard {
	struct fplinux_ui_frame_peer *compositor;
	struct wl_global *global;
	struct wl_resource *seat;
	struct wl_resource *keyboard;
	const char *name;
};

struct fplinux_ui_frame_peer {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_resource *surface;
	struct wl_resource *shell;
	struct wl_resource *toplevel;
	struct wl_resource *pending;
	struct wl_resource *held[2];
	unsigned int commits;
	int width;
	int height;
	uint16_t *pixels;
	bool configured;
	bool configure_pending;
	uint32_t configure_serial;
	bool release_frames;
	const char *keymap_path;
	struct fplinux_ui_frame_keyboard keyboards[2];
};

void fplinux_ui_frame_socketpair(int sockets[2]);
void fplinux_ui_frame_client_environment(int socket, const char *session_path);
void fplinux_ui_frame_init(struct fplinux_ui_frame_peer *peer, int socket,
			   int width, int height, const char *keymap_path);
void fplinux_ui_frame_cleanup(struct fplinux_ui_frame_peer *peer);
/* Drain prior frame traffic before applying a sequential size/state change. */
void fplinux_ui_frame_configure(struct fplinux_ui_frame_peer *peer, int width,
				int height, bool fullscreen);
uint64_t fplinux_ui_frame_now_ms(void);
void fplinux_ui_frame_pump(struct fplinux_ui_frame_peer *peer);
void fplinux_ui_frame_wait_readable(struct fplinux_ui_frame_peer *peer, int fd);
int fplinux_ui_frame_listen_session(const char *path);
int fplinux_ui_frame_accept_session(struct fplinux_ui_frame_peer *peer,
				    int listener);
struct fplinux_ui_session_message
fplinux_ui_frame_wait_message(struct fplinux_ui_frame_peer *peer, int session);
void fplinux_ui_frame_focus_keyboard(struct fplinux_ui_frame_keyboard *keyboard);
void fplinux_ui_frame_send_key(struct fplinux_ui_frame_keyboard *keyboard,
			       unsigned int code, bool pressed);
void fplinux_ui_frame_send_keymap(struct fplinux_ui_frame_keyboard *keyboard);
void fplinux_ui_frame_release_buffers(struct fplinux_ui_frame_peer *peer);

#endif
