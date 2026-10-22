/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ui-frame-peer.h"
#include "xdg-shell-server-protocol.h"

#include <assert.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

uint64_t fplinux_ui_frame_now_ms(void)
{
	struct timespec now;

	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void destroy_resource(struct wl_client *client,
			     struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void attach_buffer(struct wl_client *client, struct wl_resource *surface,
			  struct wl_resource *buffer, int32_t x, int32_t y)
{
	struct fplinux_ui_frame_peer *compositor =
		wl_resource_get_user_data(surface);

	(void)client;
	assert(x == 0 && y == 0);
	compositor->pending = buffer;
}

static void damage_surface(struct wl_client *client,
			   struct wl_resource *surface, int32_t x, int32_t y,
			   int32_t width, int32_t height)
{
	(void)client;
	(void)surface;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void configure_surface(struct fplinux_ui_frame_peer *peer,
			      uint32_t state)
{
	struct wl_array states;

	assert(peer->shell && peer->toplevel && !peer->configure_pending);
	wl_array_init(&states);
	if (state) {
		uint32_t *entry = wl_array_add(&states, sizeof(*entry));

		assert(entry);
		*entry = state;
	}
	peer->configure_serial = wl_display_next_serial(peer->display);
	peer->configure_pending = true;
	xdg_toplevel_send_configure(peer->toplevel, peer->width, peer->height,
				    &states);
	xdg_surface_send_configure(peer->shell, peer->configure_serial);
	wl_array_release(&states);
	peer->configured = true;
}

void fplinux_ui_frame_configure(struct fplinux_ui_frame_peer *peer, int width,
				int height, bool fullscreen)
{
	uint16_t *pixels;

	assert(peer->configured && !peer->configure_pending);
	assert(width > 0 && width <= INT_MAX / (int)sizeof(uint16_t));
	assert(height > 0);
	assert((size_t)width <= SIZE_MAX / sizeof(uint16_t) / (size_t)height);
	pixels = malloc((size_t)width * (size_t)height * sizeof(*pixels));
	assert(pixels);
	free(peer->pixels);
	peer->pixels = pixels;
	peer->width = width;
	peer->height = height;
	configure_surface(peer, fullscreen ? XDG_TOPLEVEL_STATE_FULLSCREEN :
					     XDG_TOPLEVEL_STATE_MAXIMIZED);
}

static void commit_surface(struct wl_client *client,
			   struct wl_resource *surface)
{
	struct fplinux_ui_frame_peer *compositor =
		wl_resource_get_user_data(surface);
	struct wl_shm_buffer *buffer;
	uint16_t *pixels;
	int stride;

	(void)client;
	if (!compositor->configured) {
		configure_surface(compositor, 0);
		return;
	}
	assert(!compositor->configure_pending);
	assert(compositor->pending);
	buffer = wl_shm_buffer_get(compositor->pending);
	assert(buffer);
	assert(wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_RGB565);
	assert(wl_shm_buffer_get_width(buffer) == compositor->width);
	assert(wl_shm_buffer_get_height(buffer) == compositor->height);
	stride = wl_shm_buffer_get_stride(buffer);
	assert(stride >= compositor->width * (int)sizeof(uint16_t));
	assert(stride % (int)sizeof(uint16_t) == 0);
	stride /= (int)sizeof(uint16_t);
	wl_shm_buffer_begin_access(buffer);
	pixels = wl_shm_buffer_get_data(buffer);
	for (int y = 0; y < compositor->height; ++y)
		memcpy(compositor->pixels + (size_t)y * compositor->width,
		       pixels + (size_t)y * stride,
		       (size_t)compositor->width * sizeof(uint16_t));
	wl_shm_buffer_end_access(buffer);
	if (compositor->release_frames)
		wl_buffer_send_release(compositor->pending);
	else if (!compositor->held[0])
		compositor->held[0] = compositor->pending;
	else {
		assert(!compositor->held[1]);
		compositor->held[1] = compositor->pending;
	}
	compositor->pending = NULL;
	++compositor->commits;
}

static const struct wl_surface_interface surface_interface = {
	.destroy = destroy_resource,
	.attach = attach_buffer,
	.damage = damage_surface,
	.commit = commit_surface,
};

static void create_surface(struct wl_client *client,
			   struct wl_resource *resource, uint32_t id)
{
	struct fplinux_ui_frame_peer *compositor =
		wl_resource_get_user_data(resource);

	assert(!compositor->surface);
	compositor->surface =
		wl_resource_create(client, &wl_surface_interface,
				   wl_resource_get_version(resource), id);
	assert(compositor->surface);
	wl_resource_set_implementation(compositor->surface, &surface_interface,
				       compositor, NULL);
}

static const struct wl_compositor_interface compositor_interface = {
	.create_surface = create_surface,
};

static void bind_compositor(struct wl_client *client, void *data,
			    uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(
		client, &wl_compositor_interface, version, id);

	assert(resource);
	wl_resource_set_implementation(resource, &compositor_interface, data,
				       NULL);
}

static void set_text(struct wl_client *client, struct wl_resource *resource,
		     const char *text)
{
	(void)client;
	(void)resource;
	(void)text;
}

static const struct xdg_toplevel_interface toplevel_interface = {
	.destroy = destroy_resource,
	.set_title = set_text,
	.set_app_id = set_text,
};

static void get_toplevel(struct wl_client *client, struct wl_resource *resource,
			 uint32_t id)
{
	struct fplinux_ui_frame_peer *compositor =
		wl_resource_get_user_data(resource);

	compositor->toplevel =
		wl_resource_create(client, &xdg_toplevel_interface, 1, id);
	assert(compositor->toplevel);
	wl_resource_set_implementation(compositor->toplevel,
				       &toplevel_interface, compositor, NULL);
}

static void ack_configure(struct wl_client *client,
			  struct wl_resource *resource, uint32_t serial)
{
	struct fplinux_ui_frame_peer *peer =
		wl_resource_get_user_data(resource);

	(void)client;
	assert(peer->configure_pending && serial == peer->configure_serial);
	peer->configure_pending = false;
}

static const struct xdg_surface_interface shell_surface_interface = {
	.destroy = destroy_resource,
	.get_toplevel = get_toplevel,
	.ack_configure = ack_configure,
};

static void get_shell_surface(struct wl_client *client,
			      struct wl_resource *resource, uint32_t id,
			      struct wl_resource *surface)
{
	struct fplinux_ui_frame_peer *compositor =
		wl_resource_get_user_data(resource);

	assert(surface == compositor->surface);
	compositor->shell =
		wl_resource_create(client, &xdg_surface_interface, 1, id);
	assert(compositor->shell);
	wl_resource_set_implementation(
		compositor->shell, &shell_surface_interface, compositor, NULL);
}

static const struct xdg_wm_base_interface shell_interface = {
	.destroy = destroy_resource,
	.get_xdg_surface = get_shell_surface,
};

static void bind_shell(struct wl_client *client, void *data, uint32_t version,
		       uint32_t id)
{
	struct wl_resource *resource =
		wl_resource_create(client, &xdg_wm_base_interface, version, id);

	assert(resource);
	wl_resource_set_implementation(resource, &shell_interface, data, NULL);
}

void fplinux_ui_frame_send_keymap(struct fplinux_ui_frame_keyboard *peer)
{
	struct xkb_context *context =
		xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES |
				XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
	FILE *source = fopen(peer->compositor->keymap_path, "r");
	struct xkb_keymap *keymap;
	char *text;
	size_t bytes;
	int fd;

	assert(context && source);
	keymap = xkb_keymap_new_from_file(context, source,
					  XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	assert(fclose(source) == 0);
	assert(keymap);
	text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	assert(text);
	bytes = strlen(text) + 1;
	assert(bytes <= UINT32_MAX);
	fd = memfd_create("ui-frame-keymap", MFD_CLOEXEC);
	assert(fd >= 0);
	assert(write(fd, text, bytes) == (ssize_t)bytes);
	wl_keyboard_send_keymap(peer->keyboard,
				WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
				(uint32_t)bytes);
	assert(close(fd) == 0);
	free(text);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

static const struct wl_keyboard_interface keyboard_interface = {
	.release = destroy_resource,
};

static void get_keyboard(struct wl_client *client, struct wl_resource *resource,
			 uint32_t id)
{
	struct fplinux_ui_frame_keyboard *peer =
		wl_resource_get_user_data(resource);

	assert(!peer->keyboard);
	peer->keyboard = wl_resource_create(client, &wl_keyboard_interface,
					    wl_resource_get_version(resource),
					    id);
	assert(peer->keyboard);
	wl_resource_set_implementation(peer->keyboard, &keyboard_interface,
				       peer, NULL);
	fplinux_ui_frame_send_keymap(peer);
	wl_keyboard_send_repeat_info(peer->keyboard, 20, 80);
}

static const struct wl_seat_interface seat_interface = {
	.get_keyboard = get_keyboard,
	.release = destroy_resource,
};

static void bind_seat(struct wl_client *client, void *data, uint32_t version,
		      uint32_t id)
{
	struct fplinux_ui_frame_keyboard *peer = data;

	assert(!peer->seat);
	peer->seat =
		wl_resource_create(client, &wl_seat_interface, version, id);
	assert(peer->seat);
	wl_resource_set_implementation(peer->seat, &seat_interface, peer, NULL);
	wl_seat_send_name(peer->seat, peer->name);
	wl_seat_send_capabilities(peer->seat, WL_SEAT_CAPABILITY_KEYBOARD);
}

static void add_keyboards(struct fplinux_ui_frame_peer *compositor)
{
	const char *names[] = { "external-keyboard", FPLINUX_UI_KEYPAD_SEAT };

	for (unsigned int i = 0; i < 2; ++i) {
		struct fplinux_ui_frame_keyboard *peer =
			&compositor->keyboards[i];

		peer->compositor = compositor;
		peer->name = names[i];
		peer->global = wl_global_create(compositor->display,
						&wl_seat_interface, 5, peer,
						bind_seat);
		assert(peer->global);
	}
}

void fplinux_ui_frame_focus_keyboard(struct fplinux_ui_frame_keyboard *peer)
{
	struct wl_array keys;

	wl_array_init(&keys);
	wl_keyboard_send_enter(
		peer->keyboard,
		wl_display_next_serial(peer->compositor->display),
		peer->compositor->surface, &keys);
	wl_array_release(&keys);
	wl_keyboard_send_modifiers(peer->keyboard, 1, 0, 0, 0, 0);
	wl_display_flush_clients(peer->compositor->display);
}

void fplinux_ui_frame_send_key(struct fplinux_ui_frame_keyboard *peer,
			       unsigned int code, bool pressed)
{
	wl_keyboard_send_key(peer->keyboard,
			     wl_display_next_serial(peer->compositor->display),
			     0xf1234567, code,
			     pressed ? WL_KEYBOARD_KEY_STATE_PRESSED :
				       WL_KEYBOARD_KEY_STATE_RELEASED);
	wl_display_flush_clients(peer->compositor->display);
}

void fplinux_ui_frame_release_buffers(struct fplinux_ui_frame_peer *compositor)
{
	for (unsigned int i = 0; i < 2; ++i)
		if (compositor->held[i]) {
			wl_buffer_send_release(compositor->held[i]);
			compositor->held[i] = NULL;
		}
	wl_display_flush_clients(compositor->display);
}

int fplinux_ui_frame_listen_session(const char *path)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	assert(listener >= 0);
	assert(strlen(path) < sizeof(address.sun_path));
	strcpy(address.sun_path, path);
	assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) ==
	       0);
	assert(listen(listener, 1) == 0);
	return listener;
}

void fplinux_ui_frame_socketpair(int sockets[2])
{
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
}

void fplinux_ui_frame_client_environment(int socket, const char *session_path)
{
	char descriptor[32];

	snprintf(descriptor, sizeof(descriptor), "%d", socket);
	assert(setenv("WAYLAND_SOCKET", descriptor, 1) == 0);
	assert(setenv("FPLINUX_UI_SESSION_SOCKET", session_path, 1) == 0);
}

void fplinux_ui_frame_init(struct fplinux_ui_frame_peer *peer, int socket,
			   int width, int height, const char *keymap_path)
{
	assert(width > 0 && width <= INT_MAX / (int)sizeof(uint16_t));
	assert(height > 0);
	assert((size_t)width <= SIZE_MAX / sizeof(uint16_t) / (size_t)height);
	*peer = (struct fplinux_ui_frame_peer){
		.width = width,
		.height = height,
		.release_frames = true,
		.keymap_path = keymap_path,
	};
	peer->pixels = malloc((size_t)width * height * sizeof(uint16_t));
	assert(peer->pixels);
	peer->display = wl_display_create();
	assert(peer->display);
	peer->loop = wl_display_get_event_loop(peer->display);
	assert(wl_display_init_shm(peer->display) == 0);
	assert(wl_display_add_shm_format(peer->display, WL_SHM_FORMAT_RGB565));
	assert(wl_global_create(peer->display, &wl_compositor_interface, 4,
				peer, bind_compositor));
	assert(wl_global_create(peer->display, &xdg_wm_base_interface, 1, peer,
				bind_shell));
	if (keymap_path)
		add_keyboards(peer);
	assert(wl_client_create(peer->display, socket));
}

void fplinux_ui_frame_cleanup(struct fplinux_ui_frame_peer *peer)
{
	wl_display_destroy_clients(peer->display);
	wl_display_destroy(peer->display);
	free(peer->pixels);
}

void fplinux_ui_frame_pump(struct fplinux_ui_frame_peer *peer)
{
	assert(wl_event_loop_dispatch(peer->loop, 5) >= 0);
	wl_display_flush_clients(peer->display);
}

void fplinux_ui_frame_wait_readable(struct fplinux_ui_frame_peer *peer, int fd)
{
	struct pollfd descriptor = { .fd = fd, .events = POLLIN };
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	int ready;

	do {
		fplinux_ui_frame_pump(peer);
		ready = poll(&descriptor, 1, 0);
		assert(ready >= 0);
	} while (!ready && fplinux_ui_frame_now_ms() < deadline);
	assert(ready == 1 && (descriptor.revents & POLLIN));
}

int fplinux_ui_frame_accept_session(struct fplinux_ui_frame_peer *peer,
				    int listener)
{
	int session;

	fplinux_ui_frame_wait_readable(peer, listener);
	session = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	assert(session >= 0);
	return session;
}

struct fplinux_ui_session_message
fplinux_ui_frame_wait_message(struct fplinux_ui_frame_peer *peer, int session)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	struct fplinux_ui_session_message message;
	int received;

	do {
		fplinux_ui_frame_pump(peer);
		received = fplinux_ui_session_receive(session, &message);
		assert(received >= 0);
	} while (!received && fplinux_ui_frame_now_ms() < deadline);
	assert(received == 1);
	return message;
}
