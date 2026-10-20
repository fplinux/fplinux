/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-ui-fonts.h"
#include "fplinux-ui-menu.h"
#include "fplinux-ui-profile.h"
#include "fplinux-ui-screen.h"
#include "fplinux-ui-session.h"
#include "fplinux-ui.h"
#include "xdg-shell-server-protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

struct compositor;

struct keyboard_peer {
	struct compositor *compositor;
	struct wl_global *global;
	struct wl_resource *seat;
	struct wl_resource *keyboard;
	const char *name;
};

struct compositor {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_resource *surface;
	struct wl_resource *shell;
	struct wl_resource *toplevel;
	struct wl_resource *pending;
	struct wl_resource *held[2];
	unsigned int commits;
	uint16_t colour;
	bool uniform;
	bool configured;
	bool release_frames;
	const char *keymap_path;
	struct keyboard_peer keyboards[2];
};

struct application {
	int control;
	bool closed;
	lv_obj_t *softkeys;
};

enum observation_type {
	OBSERVATION_KEY,
	OBSERVATION_COMMAND,
	OBSERVATION_VISIBILITY,
	OBSERVATION_CLOSE,
	OBSERVATION_INPUT_CANCEL,
};

struct observation {
	enum observation_type type;
	struct fplinux_ui_key key;
	char command;
	bool visible;
	bool menu_open;
};

struct repeat_application {
	int control;
	bool closed;
	bool bounce_on_press;
};

/* Generated package fonts are outside the frame/transport boundary. */
const lv_font_t *fplinux_ui_builtin_font(const char *id)
{
	(void)id;
	return &lv_font_montserrat_14;
}

static uint64_t now_ms(void)
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
	struct compositor *compositor = wl_resource_get_user_data(surface);

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

static void commit_surface(struct wl_client *client,
			   struct wl_resource *surface)
{
	struct compositor *compositor = wl_resource_get_user_data(surface);
	struct wl_shm_buffer *buffer;
	uint16_t *pixels;
	int stride;

	(void)client;
	if (!compositor->configured) {
		struct wl_array states;

		assert(compositor->shell && compositor->toplevel);
		wl_array_init(&states);
		xdg_toplevel_send_configure(compositor->toplevel, 240, 320,
					    &states);
		xdg_surface_send_configure(compositor->shell, 1);
		wl_array_release(&states);
		compositor->configured = true;
		return;
	}
	assert(compositor->pending);
	buffer = wl_shm_buffer_get(compositor->pending);
	assert(buffer);
	assert(wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_RGB565);
	assert(wl_shm_buffer_get_width(buffer) == 240);
	assert(wl_shm_buffer_get_height(buffer) == 320);
	stride = wl_shm_buffer_get_stride(buffer) / 2;
	wl_shm_buffer_begin_access(buffer);
	pixels = wl_shm_buffer_get_data(buffer);
	compositor->colour = pixels[0];
	compositor->uniform = true;
	for (int y = 0; y < 320; ++y)
		for (int x = 0; x < 240; ++x)
			compositor->uniform &= pixels[y * stride + x] ==
					       compositor->colour;
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
	struct compositor *compositor = wl_resource_get_user_data(resource);

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
	struct compositor *compositor = wl_resource_get_user_data(resource);

	compositor->toplevel =
		wl_resource_create(client, &xdg_toplevel_interface, 1, id);
	assert(compositor->toplevel);
	wl_resource_set_implementation(compositor->toplevel,
				       &toplevel_interface, compositor, NULL);
}

static void ack_configure(struct wl_client *client,
			  struct wl_resource *resource, uint32_t serial)
{
	(void)client;
	(void)resource;
	assert(serial == 1);
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
	struct compositor *compositor = wl_resource_get_user_data(resource);

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

static void send_keymap(struct keyboard_peer *peer)
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
	assert(xkb_keymap_mod_get_index(keymap, "Shift") == 0);
	text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	assert(text);
	bytes = strlen(text) + 1;
	assert(bytes <= UINT32_MAX);
	fd = memfd_create("ui-repeat-keymap", MFD_CLOEXEC);
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
	struct keyboard_peer *peer = wl_resource_get_user_data(resource);

	assert(!peer->keyboard);
	peer->keyboard = wl_resource_create(client, &wl_keyboard_interface,
					    wl_resource_get_version(resource),
					    id);
	assert(peer->keyboard);
	wl_resource_set_implementation(peer->keyboard, &keyboard_interface,
				       peer, NULL);
	send_keymap(peer);
	wl_keyboard_send_repeat_info(peer->keyboard, 20, 80);
}

static const struct wl_seat_interface seat_interface = {
	.get_keyboard = get_keyboard,
	.release = destroy_resource,
};

static void bind_seat(struct wl_client *client, void *data, uint32_t version,
		      uint32_t id)
{
	struct keyboard_peer *peer = data;

	assert(!peer->seat);
	peer->seat =
		wl_resource_create(client, &wl_seat_interface, version, id);
	assert(peer->seat);
	wl_resource_set_implementation(peer->seat, &seat_interface, peer, NULL);
	wl_seat_send_name(peer->seat, peer->name);
	wl_seat_send_capabilities(peer->seat, WL_SEAT_CAPABILITY_KEYBOARD);
}

static void add_keyboards(struct compositor *compositor)
{
	const char *names[] = { "external-keyboard", FPLINUX_UI_KEYPAD_SEAT };

	for (unsigned int i = 0; i < 2; ++i) {
		struct keyboard_peer *peer = &compositor->keyboards[i];

		peer->compositor = compositor;
		peer->name = names[i];
		peer->global = wl_global_create(compositor->display,
						&wl_seat_interface, 5, peer,
						bind_seat);
		assert(peer->global);
	}
}

static void paint(struct fplinux_ui *ui, uint32_t colour)
{
	lv_obj_set_style_bg_color(fplinux_ui_screen(ui), lv_color_hex(colour),
				  0);
}

static uint32_t paused_tick(void)
{
	/* Keep a refresh timer which is not due from hiding a queued stale canvas. */
	return 1;
}

static void application_event(struct fplinux_ui *ui,
			      const struct fplinux_ui_event *event, void *data)
{
	struct application *application = data;

	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		application->closed = true;
		return;
	}
	if (event->type != FPLINUX_UI_EVENT_SESSION)
		return;
	if (event->session->type == FPLINUX_UI_SESSION_ACTIVE) {
		struct fplinux_ui_session_message ack = {
			.type = FPLINUX_UI_SESSION_ACTIVE_ACK,
			.serial = event->session->serial,
		};

		paint(ui, 0x00ff00);
		assert(fplinux_ui_send(ui, &ack));
	} else if (event->session->type == FPLINUX_UI_SESSION_SETTING) {
		if (!strcmp(event->session->name, "locale"))
			assert(!strcmp(fplinux_ui_locale(ui), "ru"));
		else
			assert(fplinux_ui_theme(ui)->kind ==
			       FPLINUX_UI_THEME_DARK);
		paint(ui, 0x00ff00);
	} else if (event->session->type == FPLINUX_UI_SESSION_ERROR) {
		/* The application renders a visible notice in its normal callback. */
		paint(ui, 0xffff00);
	}
	assert(write(application->control, "a", 1) == 1);
}

static void create_softkeys(struct fplinux_ui *ui,
			    struct application *application)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_profile_spec(fplinux_ui_metrics(ui)->profile);
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(ui, NULL);
	lv_area_t bar_area;
	lv_area_t labels[3];
	const char *legends[3] = { "Tasks", "Menu", "Back" };

	assert(canvas && spec);
	fplinux_ui_push(ui, canvas);
	application->softkeys = fplinux_ui_softkey_bar_create(
		ui, fplinux_ui_canvas_object(canvas));
	assert(application->softkeys);
	/* The launcher centres within the full bar, including its divider. */
	lv_obj_set_style_pad_top(application->softkeys, -spec->divider_width,
				 0);
	assert(fplinux_ui_softkey_bar_set(ui, application->softkeys, "Tasks",
					  "Menu", "Back"));
	lv_obj_update_layout(application->softkeys);
	lv_obj_get_coords(application->softkeys, &bar_area);
	/* The 240x320 profile has a 32-pixel bar and 8-pixel legend margins. */
	assert(bar_area.x1 == 0 && bar_area.x2 == 239);
	assert(bar_area.y1 == 288 && bar_area.y2 == 319);
	for (int i = 0; i < 3; ++i) {
		lv_obj_t *label = lv_obj_get_child(application->softkeys, i);

		assert(!strcmp(lv_label_get_text(label), legends[i]));
		lv_obj_get_coords(label, &labels[i]);
		assert(abs(labels[i].y1 + labels[i].y2 - bar_area.y1 -
			   bar_area.y2) <= 1);
	}
	assert(labels[0].x1 == 8);
	assert(abs(labels[1].x1 + labels[1].x2 - 239) <= 1);
	assert(labels[2].x2 == 231);
}

static void application_input(struct fplinux_ui *ui, int fd, short events,
			      void *data)
{
	struct application *application = data;
	char command;
	char result = 'a';

	assert(events == POLLIN);
	assert(read(fd, &command, 1) == 1);
	if (command == 's' || command == 'u') {
		if (command == 's')
			create_softkeys(ui, application);
		else {
			assert(application->softkeys);
			assert(fplinux_ui_softkey_bar_set(
				ui, application->softkeys, "Tasks", "Menu",
				"Back"));
		}
		lv_refr_now(NULL);
		assert(write(fd, &result, 1) == 1);
		return;
	}
	if (command == 'x') {
		/* The manager disappears immediately before the user's Back action. */
		assert(write(fd, "r", 1) == 1);
		assert(read(fd, &command, 1) == 1 && command == 'b');
	}
	paint(ui, command == 'q' ? 0xff00ff : 0x0000ff);
	lv_refr_now(NULL);
	if (command == 'b')
		result = fplinux_ui_back(ui) ? 'b' : 'f';
	else
		assert(command == 'p' || command == 'q');
	assert(write(fd, &result, 1) == 1);
}

static int run_application(int control)
{
	struct application application = { .control = control };
	struct fplinux_ui *ui = fplinux_ui_open(
		"org.fplinux.frame-test", application_event, &application);
	int result;

	assert(ui);
	lv_tick_set_cb(paused_tick);
	paint(ui, 0xff0000);
	lv_refr_now(NULL);
	fplinux_ui_watch_fd(ui, control, POLLIN, application_input,
			    &application);
	result = fplinux_ui_run(ui);
	fplinux_ui_close(ui);
	assert(application.closed);
	return result;
}

static void send_observation(int control, const struct observation *observation)
{
	assert(write(control, observation, sizeof(*observation)) ==
	       (ssize_t)sizeof(*observation));
}

static void repeat_canvas_key(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *canvas,
			      const struct fplinux_ui_key *key, void *data)
{
	struct repeat_application *application = data;
	struct observation observation = {
		.type = OBSERVATION_KEY,
		.key = *key,
		.visible = fplinux_ui_is_visible(ui),
	};

	(void)canvas;
	if (application->bounce_on_press && key->pressed) {
		struct fplinux_ui_screen *cover =
			fplinux_ui_canvas_new(ui, NULL);

		assert(cover);
		application->bounce_on_press = false;
		fplinux_ui_push(ui, cover);
		assert(fplinux_ui_pop(ui));
	}
	send_observation(application->control, &observation);
}

static struct fplinux_ui_screen *
new_repeat_canvas(struct fplinux_ui *ui, struct repeat_application *application)
{
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(ui, NULL);

	assert(canvas);
	fplinux_ui_canvas_set_key_handler(canvas, repeat_canvas_key,
					  application);
	return canvas;
}

static void repeat_application_event(struct fplinux_ui *ui,
				     const struct fplinux_ui_event *event,
				     void *data)
{
	struct repeat_application *application = data;
	struct observation observation = {
		.type = OBSERVATION_VISIBILITY,
		.visible = event->visible,
	};

	(void)ui;
	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		application->closed = true;
		observation.type = OBSERVATION_CLOSE;
		send_observation(application->control, &observation);
	} else if (event->type == FPLINUX_UI_EVENT_INPUT_CANCEL) {
		observation.type = OBSERVATION_INPUT_CANCEL;
		observation.key.source = event->key.source;
		send_observation(application->control, &observation);
	} else if (event->type == FPLINUX_UI_EVENT_VISIBILITY)
		send_observation(application->control, &observation);
}

static void repeat_application_input(struct fplinux_ui *ui, int fd,
				     short events, void *data)
{
	struct repeat_application *application = data;
	struct fplinux_ui_screen *canvas = fplinux_ui_current(ui);
	struct observation observation = { .type = OBSERVATION_COMMAND };
	char command;

	assert(events == POLLIN);
	assert(read(fd, &command, 1) == 1);
	switch (command) {
	case 'c':
		assert(!canvas);
		fplinux_ui_push(ui, new_repeat_canvas(ui, application));
		break;
	case 'e':
		fplinux_ui_canvas_set_key_repeat(canvas, true);
		break;
	case 'd':
		fplinux_ui_canvas_set_key_repeat(canvas, false);
		break;
	case 'w':
		fplinux_ui_canvas_set_keyboard_keys(canvas, true);
		break;
	case 'p':
	case 'v':
		canvas = new_repeat_canvas(ui, application);
		fplinux_ui_canvas_set_key_repeat(canvas, true);
		if (command == 'p')
			fplinux_ui_push(ui, canvas);
		else
			fplinux_ui_replace(ui, canvas);
		break;
	case 'b':
		assert(fplinux_ui_pop(ui));
		break;
	case 'o': {
		const struct fplinux_ui_command help = {
			.id = 7,
			.type = FPLINUX_UI_COMMAND_HELP,
		};

		assert(fplinux_ui_screen_add_command(canvas, &help));
		break;
	}
	case 'h':
		assert(fplinux_ui_home(ui));
		break;
	case 'j':
		application->bounce_on_press = true;
		break;
	case 'z':
		/* A controlled callback pause makes several deadlines elapse. */
		observation.command = command;
		send_observation(fd, &observation);
		assert(read(fd, &command, 1) == 1 && command == 'Z');
		/* Keep the next request ready when timer dispatch resumes. */
		{
			char next;

			assert(recv(fd, &next, 1, MSG_PEEK) == 1 &&
			       next == 'n');
		}
		break;
	case 'm':
	case 'n':
		break;
	default:
		assert(false);
	}
	observation.command = command;
	observation.menu_open = fplinux_ui_menu_is_open(ui);
	send_observation(fd, &observation);
}

static int run_repeat_application(int control)
{
	struct repeat_application application = { .control = control };
	struct fplinux_ui *ui = fplinux_ui_open("org.fplinux.repeat-test",
						repeat_application_event,
						&application);
	int result;

	assert(ui);
	paint(ui, 0xff0000);
	lv_refr_now(NULL);
	fplinux_ui_watch_fd(ui, control, POLLIN, repeat_application_input,
			    &application);
	result = fplinux_ui_run(ui);
	fplinux_ui_close(ui);
	assert(application.closed);
	return result;
}

static void pump(struct compositor *compositor)
{
	assert(wl_event_loop_dispatch(compositor->loop, 5) >= 0);
	wl_display_flush_clients(compositor->display);
}

static void wait_commits(struct compositor *compositor, unsigned int count)
{
	uint64_t deadline = now_ms() + 3000;

	while (compositor->commits < count && now_ms() < deadline)
		pump(compositor);
	assert(compositor->commits == count);
}

static void assert_quiet(struct compositor *compositor, int session,
			 unsigned int count)
{
	uint64_t deadline = now_ms() + 100;
	struct fplinux_ui_session_message message;

	while (now_ms() < deadline) {
		pump(compositor);
		assert(compositor->commits == count);
		assert(fplinux_ui_session_receive(session, &message) == 0);
	}
}

static struct fplinux_ui_session_message
wait_message(struct compositor *compositor, int session,
	     enum fplinux_ui_session_message_type type)
{
	uint64_t deadline = now_ms() + 3000;
	struct fplinux_ui_session_message message;
	int received;

	do {
		pump(compositor);
		received = fplinux_ui_session_receive(session, &message);
		assert(received >= 0);
		if (received)
			assert(message.type == type);
	} while (!received && now_ms() < deadline);
	assert(received == 1);
	return message;
}

static void wait_input(struct compositor *compositor, int control,
		       char expected)
{
	struct pollfd descriptor = { .fd = control, .events = POLLIN };
	uint64_t deadline = now_ms() + 3000;
	char result;
	int ready;

	do {
		pump(compositor);
		ready = poll(&descriptor, 1, 0);
		assert(ready >= 0);
	} while (!ready && now_ms() < deadline);
	assert(ready == 1);
	assert(read(control, &result, 1) == 1);
	assert(result == expected);
}

static struct observation wait_observation(struct compositor *compositor,
					   int control)
{
	struct pollfd descriptor = { .fd = control, .events = POLLIN };
	uint64_t deadline = now_ms() + 3000;
	struct observation observation;
	int ready;

	do {
		pump(compositor);
		ready = poll(&descriptor, 1, 0);
		assert(ready >= 0);
	} while (!ready && now_ms() < deadline);
	assert(ready == 1);
	assert(read(control, &observation, sizeof(observation)) ==
	       (ssize_t)sizeof(observation));
	return observation;
}

static void assert_key(const struct observation *observation,
		       enum fplinux_ui_key_source source, unsigned int code,
		       bool pressed, const char *text)
{
	assert(observation->type == OBSERVATION_KEY);
	assert(observation->key.source == source);
	assert(observation->key.code == code);
	assert(observation->key.pressed == pressed);
	assert(!strcmp(observation->key.text, text));
}

static struct fplinux_ui_key wait_key(struct compositor *compositor,
				      int control,
				      enum fplinux_ui_key_source source,
				      unsigned int code, bool pressed,
				      const char *text)
{
	struct observation observation = wait_observation(compositor, control);

	assert_key(&observation, source, code, pressed, text);
	return observation.key;
}

static void assert_no_input(struct compositor *compositor, int control,
			    unsigned int duration_ms)
{
	struct pollfd descriptor = { .fd = control, .events = POLLIN };
	uint64_t deadline = now_ms() + duration_ms;

	while (now_ms() < deadline) {
		pump(compositor);
		assert(poll(&descriptor, 1, 0) == 0);
	}
}

static struct observation wait_command(struct compositor *compositor,
				       int control, char command)
{
	uint64_t deadline = now_ms() + 3000;
	struct observation observation;

	do {
		assert(now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.type == OBSERVATION_COMMAND) {
			assert(observation.command == command);
			return observation;
		}
		/* A key callback delivered before the command is still valid. */
		assert(observation.type == OBSERVATION_KEY);
		assert(observation.key.source == FPLINUX_UI_KEY_KEYBOARD);
		assert(observation.key.code == KEY_A);
	} while (true);
}

static struct observation application_command(struct compositor *compositor,
					      int control, char command)
{
	assert(write(control, &command, 1) == 1);
	return wait_command(compositor, control, command);
}

static void focus_keyboard(struct keyboard_peer *peer)
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

static void send_key(struct keyboard_peer *peer, unsigned int code,
		     bool pressed)
{
	wl_keyboard_send_key(peer->keyboard,
			     wl_display_next_serial(peer->compositor->display),
			     0xf1234567, code,
			     pressed ? WL_KEYBOARD_KEY_STATE_PRESSED :
				       WL_KEYBOARD_KEY_STATE_RELEASED);
	wl_display_flush_clients(peer->compositor->display);
}

static void release_key(struct compositor *compositor, int control,
			struct keyboard_peer *peer,
			enum fplinux_ui_key_source source, unsigned int code,
			const char *text)
{
	uint64_t deadline = now_ms() + 3000;
	struct observation observation;

	send_key(peer, code, false);
	while (true) {
		assert(now_ms() < deadline);
		observation = wait_observation(compositor, control);
		assert(observation.type == OBSERVATION_KEY);
		if (!observation.key.pressed) {
			assert_key(&observation, source, code, false, text);
			return;
		}
		assert_key(&observation, source, code, true, text);
	}
}

static void start_keyboard_repeat(struct compositor *compositor, int control)
{
	uint64_t sent_at = now_ms();
	struct fplinux_ui_key initial;
	struct fplinux_ui_key repeated;

	send_key(&compositor->keyboards[0], KEY_A, true);
	initial = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
			   true, "a");
	repeated = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
			    true, "a");
	assert(initial.time_ms >= sent_at &&
	       initial.time_ms <= repeated.time_ms);
	assert(repeated.time_ms <= now_ms());
}

/* A nonrepeating key after a producer update confirms its ordered delivery. */
static void keyboard_barrier(struct compositor *compositor, int control,
			     struct keyboard_peer *peer,
			     enum fplinux_ui_key_source source)
{
	uint64_t deadline = now_ms() + 3000;
	struct observation observation;

	send_key(peer, KEY_F24, true);
	do {
		assert(now_ms() < deadline);
		observation = wait_observation(compositor, control);
		assert(observation.type == OBSERVATION_KEY);
		if (observation.key.source == source &&
		    observation.key.code == KEY_F24)
			break;
		assert(observation.key.source == FPLINUX_UI_KEY_KEYBOARD);
		assert(observation.key.code == KEY_A);
	} while (true);
	assert_key(&observation, source, KEY_F24, true, "");
	send_key(peer, KEY_F24, false);
	wait_key(compositor, control, source, KEY_F24, false, "");
}

static void check_keyboard_repeat(struct compositor *compositor, int control)
{
	struct keyboard_peer *keyboard = &compositor->keyboards[0];
	struct fplinux_ui_key key;
	struct observation observation;
	uint64_t deadline;

	/* Neither a default Canvas nor raw Enter/Escape handling opts into repeat. */
	for (unsigned int raw = 0; raw < 2; ++raw) {
		if (raw)
			application_command(compositor, control, 'w');
		send_key(keyboard, KEY_A, true);
		key = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD,
			       KEY_A, true, "a");
		assert(key.keysym == 'a' && key.unicode == 'a' &&
		       key.ascii == 'a');
		assert_no_input(compositor, control, 240);
		send_key(keyboard, KEY_A, false);
		wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
			 false, "a");
	}
	application_command(compositor, control, 'e');
	start_keyboard_repeat(compositor, control);

	/* A held letter resolves fresh modifiers without losing its repeat. */
	wl_keyboard_send_modifiers(keyboard->keyboard, 2, 1, 0, 0, 0);
	send_key(keyboard, KEY_LEFTSHIFT, true);
	deadline = now_ms() + 3000;
	do {
		assert(now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.key.code == KEY_LEFTSHIFT)
			break;
		assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
			   "a");
	} while (true);
	assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_LEFTSHIFT, true,
		   "");
	assert(observation.key.shift);
	key = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
		       true, "A");
	assert(key.shift && !key.control && !key.alt);
	assert(key.keysym == 'A' && key.unicode == 'A' && key.ascii == 'a');

	/* Group 2 uses a distinct literal, so cached text cannot satisfy this. */
	wl_keyboard_send_modifiers(keyboard->keyboard, 3, 1, 0, 0, 1);
	send_key(keyboard, KEY_LEFTSHIFT, false);
	deadline = now_ms() + 3000;
	do {
		assert(now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.key.code == KEY_LEFTSHIFT)
			break;
		assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
			   "A");
	} while (true);
	assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_LEFTSHIFT, false,
		   "");
	key = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
		       true, "B");
	assert(key.shift && key.keysym == 'B' && key.unicode == 'B');
	release_key(compositor, control, keyboard, FPLINUX_UI_KEY_KEYBOARD,
		    KEY_A, "B");
	assert_no_input(compositor, control, 240);

	/* The keymap marks Shift nonrepeating even on an opted-in Canvas. */
	send_key(keyboard, KEY_LEFTSHIFT, true);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_LEFTSHIFT,
		 true, "");
	assert_no_input(compositor, control, 240);
	send_key(keyboard, KEY_LEFTSHIFT, false);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_LEFTSHIFT,
		 false, "");
}

static void check_phone_repeat(struct compositor *compositor, int control)
{
	struct keyboard_peer *phone = &compositor->keyboards[1];
	const unsigned int arrows[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
	const unsigned int single[] = { KEY_NUMERIC_2,	   KEY_NUMERIC_STAR,
					KEY_NUMERIC_POUND, KEY_OK,
					KEY_F13,	   KEY_F14 };

	application_command(compositor, control, 'e');
	for (unsigned int i = 0; i < sizeof(arrows) / sizeof(arrows[0]); ++i) {
		send_key(phone, arrows[i], true);
		wait_key(compositor, control, FPLINUX_UI_KEY_PHONE, arrows[i],
			 true, "");
		wait_key(compositor, control, FPLINUX_UI_KEY_PHONE, arrows[i],
			 true, "");
		release_key(compositor, control, phone, FPLINUX_UI_KEY_PHONE,
			    arrows[i], "");
		assert_no_input(compositor, control, 120);
	}
	for (unsigned int i = 0; i < sizeof(single) / sizeof(single[0]); ++i) {
		send_key(phone, single[i], true);
		wait_key(compositor, control, FPLINUX_UI_KEY_PHONE, single[i],
			 true, "");
		assert_no_input(compositor, control, 240);
		send_key(phone, single[i], false);
		wait_key(compositor, control, FPLINUX_UI_KEY_PHONE, single[i],
			 false, "");
	}
}

static void wait_menu(struct compositor *compositor, int control, bool open)
{
	uint64_t deadline = now_ms() + 3000;

	while (application_command(compositor, control, 'm').menu_open != open)
		assert(now_ms() < deadline);
}

static void wait_cancelled_keyboard(struct compositor *compositor, int control)
{
	struct observation observation;
	uint64_t deadline = now_ms() + 3000;

	for (;;) {
		assert(now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.type == OBSERVATION_INPUT_CANCEL) {
			assert(observation.key.source ==
			       FPLINUX_UI_KEY_KEYBOARD);
			break;
		}
		assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
			   "a");
	}
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A, false,
		 "");
}

static void check_repeat_cancellation(struct compositor *compositor,
				      int session, int control,
				      const char *scenario)
{
	struct keyboard_peer *keyboard = &compositor->keyboards[0];
	struct keyboard_peer *phone = &compositor->keyboards[1];
	struct fplinux_ui_session_message visibility = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
	};
	struct observation observation;
	uint64_t deadline;
	bool cancelled[2] = { false, false };

	application_command(compositor, control, 'e');
	if (!strcmp(scenario, "repeat-options"))
		application_command(compositor, control, 'o');
	start_keyboard_repeat(compositor, control);
	if (!strcmp(scenario, "repeat-leave")) {
		wl_keyboard_send_leave(keyboard->keyboard, 2,
				       compositor->surface);
		wait_cancelled_keyboard(compositor, control);
		keyboard_barrier(compositor, control, phone,
				 FPLINUX_UI_KEY_PHONE);
		assert_no_input(compositor, control, 240);
		focus_keyboard(keyboard);
		keyboard_barrier(compositor, control, phone,
				 FPLINUX_UI_KEY_PHONE);
	} else if (!strcmp(scenario, "repeat-visibility")) {
		assert(fplinux_ui_session_send(session, &visibility));
		deadline = now_ms() + 3000;
		do {
			assert(now_ms() < deadline);
			observation = wait_observation(compositor, control);
			if (observation.type == OBSERVATION_VISIBILITY)
				break;
			if (observation.type == OBSERVATION_INPUT_CANCEL) {
				assert(observation.key.source ==
					       FPLINUX_UI_KEY_PHONE ||
				       observation.key.source ==
					       FPLINUX_UI_KEY_KEYBOARD);
				cancelled[observation.key.source] = true;
				continue;
			}
			assert(observation.type == OBSERVATION_KEY);
			assert(observation.key.code == KEY_A);
			if (!observation.key.pressed) {
				assert(cancelled[FPLINUX_UI_KEY_KEYBOARD]);
				assert(!observation.visible);
			}
		} while (true);
		assert(!observation.visible);
		assert(cancelled[FPLINUX_UI_KEY_PHONE] &&
		       cancelled[FPLINUX_UI_KEY_KEYBOARD]);
		assert_no_input(compositor, control, 240);
		visibility.visible = true;
		assert(fplinux_ui_session_send(session, &visibility));
		observation = wait_observation(compositor, control);
		assert(observation.type == OBSERVATION_VISIBILITY &&
		       observation.visible);
	} else if (!strcmp(scenario, "repeat-options")) {
		send_key(keyboard, KEY_COMPOSE, true);
		send_key(keyboard, KEY_COMPOSE, false);
		wait_menu(compositor, control, true);
		assert_no_input(compositor, control, 240);
		send_key(keyboard, KEY_ESC, true);
		send_key(keyboard, KEY_ESC, false);
		wait_menu(compositor, control, false);
	} else if (!strcmp(scenario, "repeat-stack")) {
		application_command(compositor, control, 'p');
		assert_no_input(compositor, control, 240);
		application_command(compositor, control, 'b');
	} else if (!strcmp(scenario, "repeat-replace")) {
		application_command(compositor, control, 'v');
	} else if (!strcmp(scenario, "repeat-disable")) {
		application_command(compositor, control, 'd');
		assert_no_input(compositor, control, 240);
		application_command(compositor, control, 'e');
	} else if (!strcmp(scenario, "repeat-keymap")) {
		send_keymap(keyboard);
		keyboard_barrier(compositor, control, keyboard,
				 FPLINUX_UI_KEY_KEYBOARD);
	} else if (!strcmp(scenario, "repeat-seat")) {
		wl_global_destroy(keyboard->global);
		keyboard->global = NULL;
		wait_cancelled_keyboard(compositor, control);
		keyboard_barrier(compositor, control, phone,
				 FPLINUX_UI_KEY_PHONE);
	} else if (!strcmp(scenario, "repeat-rate-zero")) {
		wl_keyboard_send_repeat_info(keyboard->keyboard, 0, 80);
		keyboard_barrier(compositor, control, keyboard,
				 FPLINUX_UI_KEY_KEYBOARD);
		assert_no_input(compositor, control, 240);
		wl_keyboard_send_repeat_info(keyboard->keyboard, 20, 80);
		keyboard_barrier(compositor, control, keyboard,
				 FPLINUX_UI_KEY_KEYBOARD);
	} else {
		assert(!strcmp(scenario, "repeat-home"));
		application_command(compositor, control, 'h');
	}
	/* Returning focus, Back or reenabling repeat cannot revive the old hold. */
	assert_no_input(compositor, control, 240);
}

static void check_repeat_pause(struct compositor *compositor, int control)
{
	struct observation observation;
	unsigned int repeats = 0;
	uint64_t deadline;

	application_command(compositor, control, 'e');
	start_keyboard_repeat(compositor, control);
	application_command(compositor, control, 'z');
	assert_no_input(compositor, control, 400);
	assert(write(control, "Z", 1) == 1);
	assert(write(control, "n", 1) == 1);
	wait_command(compositor, control, 'Z');
	deadline = now_ms() + 3000;
	do {
		assert(now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.type == OBSERVATION_COMMAND)
			break;
		assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
			   "a");
		++repeats;
	} while (true);
	assert(observation.command == 'n');
	/* Eight overdue intervals must not arrive as a catch-up burst. */
	assert(repeats <= 2);
	if (!repeats)
		wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
			 true, "a");
	release_key(compositor, control, &compositor->keyboards[0],
		    FPLINUX_UI_KEY_KEYBOARD, KEY_A, "a");
	assert_no_input(compositor, control, 240);
}

static void check_repeat_bounce(struct compositor *compositor, int control)
{
	application_command(compositor, control, 'e');
	application_command(compositor, control, 'j');
	send_key(&compositor->keyboards[0], KEY_A, true);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
		 "a");
	/* The first handler returned to the same Canvas after a push and Back. */
	assert_no_input(compositor, control, 240);
	send_key(&compositor->keyboards[0], KEY_A, false);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A, false,
		 "a");
}

static void check_repeat_closed(struct compositor *compositor, int control,
				const char *scenario)
{
	struct observation observation;
	uint64_t deadline = now_ms() + 3000;

	do {
		assert(now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.type == OBSERVATION_CLOSE)
			break;
		assert(!strcmp(scenario, "repeat-close"));
		assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
			   "a");
	} while (true);
	assert(read(control, &observation, sizeof(observation)) == 0);
}

static void check_repeat(struct compositor *compositor, int session,
			 int control, const char *scenario)
{
	assert(compositor->keyboards[0].keyboard);
	assert(compositor->keyboards[1].keyboard);
	focus_keyboard(&compositor->keyboards[0]);
	focus_keyboard(&compositor->keyboards[1]);
	application_command(compositor, control, 'c');
	if (!strcmp(scenario, "repeat-keyboard"))
		check_keyboard_repeat(compositor, control);
	else if (!strcmp(scenario, "repeat-phone"))
		check_phone_repeat(compositor, control);
	else if (!strcmp(scenario, "repeat-pause"))
		check_repeat_pause(compositor, control);
	else if (!strcmp(scenario, "repeat-bounce"))
		check_repeat_bounce(compositor, control);
	else if (!strcmp(scenario, "repeat-close")) {
		application_command(compositor, control, 'e');
		start_keyboard_repeat(compositor, control);
	} else
		check_repeat_cancellation(compositor, session, control,
					  scenario);
}

static void release_buffers(struct compositor *compositor)
{
	for (unsigned int i = 0; i < 2; ++i)
		if (compositor->held[i]) {
			wl_buffer_send_release(compositor->held[i]);
			compositor->held[i] = NULL;
		}
	wl_display_flush_clients(compositor->display);
}

static void check_softkeys(struct compositor *compositor, int session,
			   int control)
{
	struct fplinux_ui_session_message legends;

	assert(write(control, "s", 1) == 1);
	wait_input(compositor, control, 'a');
	/* Pushing the Canvas reports its empty commands; its drawn bar sends none. */
	legends =
		wait_message(compositor, session, FPLINUX_UI_SESSION_SOFTKEYS);
	assert(!legends.left[0] && !legends.centre[0] && !legends.right[0]);
	wait_commits(compositor, 2);
	release_buffers(compositor);
	assert(write(control, "u", 1) == 1);
	wait_input(compositor, control, 'a');
	assert_quiet(compositor, session, 2);
}

static void check_exit(struct compositor *compositor, int session, int control,
		       pid_t child)
{
	unsigned int retained = compositor->commits;
	int status;

	release_buffers(compositor);
	assert(write(control, "b", 1) == 1);
	wait_input(compositor, control, 'b');
	wait_message(compositor, session, FPLINUX_UI_SESSION_EXIT);
	assert_quiet(compositor, session, retained);
	assert(waitpid(child, &status, WNOHANG) == 0);
	assert(compositor->colour == 0xf800 && compositor->uniform);
}

static void check_ack(struct compositor *compositor, int session, int control,
		      const char *scenario)
{
	struct fplinux_ui_session_message update = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.serial = 73,
	};
	struct fplinux_ui_session_message ack;
	enum fplinux_ui_session_message_type expected =
		FPLINUX_UI_SESSION_REDRAW_ACK;
	uint16_t colour = 0x07e0;

	/* Both client buffers become busy with actual, differently drawn frames. */
	assert(write(control, "p", 1) == 1);
	wait_input(compositor, control, 'a');
	wait_commits(compositor, 2);
	assert(compositor->colour == 0x001f && compositor->uniform);
	/* A third, completed old canvas is queued while both buffers are busy. */
	assert(write(control, "q", 1) == 1);
	wait_input(compositor, control, 'a');
	if (!strcmp(scenario, "active")) {
		update.type = FPLINUX_UI_SESSION_ACTIVE;
		strcpy(update.locale, "en");
		strcpy(update.input_locale, "en");
		strcpy(update.value, "home");
		strcpy(update.name, "grid");
		update.battery = -1;
		expected = FPLINUX_UI_SESSION_ACTIVE_ACK;
	} else if (!strcmp(scenario, "error")) {
		update.type = FPLINUX_UI_SESSION_ERROR;
		strcpy(update.reason, "save-failed");
		colour = 0xffe0;
	} else {
		assert(!strcmp(scenario, "setting") ||
		       !strcmp(scenario, "locale"));
		strcpy(update.name,
		       !strcmp(scenario, "locale") ? "locale" : "theme");
		strcpy(update.value,
		       !strcmp(scenario, "locale") ? "ru" : "dark");
	}
	assert(fplinux_ui_session_send(session, &update));
	wait_input(compositor, control, 'a');
	assert_quiet(compositor, session, 2);
	release_buffers(compositor);
	{
		struct pollfd descriptor = { .fd = session, .events = POLLIN };

		/* The new buffer can commit, but the server has not handled its sync. */
		assert(poll(&descriptor, 1, 100) == 0);
	}
	ack = wait_message(compositor, session, expected);
	assert(ack.serial == 73);
	assert(compositor->commits == 3);
	assert(compositor->colour == colour && compositor->uniform);
}

static int listen_session(const char *path)
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

static void wait_application(struct compositor *compositor, pid_t child,
			     int exit_status)
{
	uint64_t deadline = now_ms() + 3000;
	int status;
	pid_t finished;

	do {
		pump(compositor);
		finished = waitpid(child, &status, WNOHANG);
		assert(finished >= 0);
	} while (!finished && now_ms() < deadline);
	assert(finished == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == exit_status);
}

static void finish_application(struct compositor *compositor, int session,
			       pid_t child)
{
	struct fplinux_ui_session_message close_message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};

	strcpy(close_message.reason, "test-complete");
	assert(fplinux_ui_session_send(session, &close_message));
	wait_application(compositor, child, 0);
}

static void check_failed_exit(struct compositor *compositor, int session,
			      int control, pid_t child)
{
	assert(write(control, "x", 1) == 1);
	wait_input(compositor, control, 'r');
	assert(close(session) == 0);
	assert(write(control, "b", 1) == 1);
	wait_input(compositor, control, 'f');
	/* A failed protocol send ends the connection and must reach CLOSE locally. */
	wait_application(compositor, child, 255);
}

int main(int argc, char **argv)
{
	struct compositor compositor = { 0 };
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_LAUNCHER,
		.screen_width = 240,
		.screen_height = 320,
	};
	int wayland[2], control[2];
	int listener, session;
	char descriptor[32];
	pid_t child;
	bool repeat;

	assert(argc == 3 || argc == 4);
	repeat = !strncmp(argv[1], "repeat-", 7);
	assert(repeat == (argc == 4));
	compositor.release_frames = repeat;
	compositor.keymap_path = repeat ? argv[3] : NULL;
	listener = listen_session(argv[2]);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, wayland) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, control) == 0);
	child = fork();
	assert(child >= 0);
	if (!child) {
		close(listener);
		close(wayland[0]);
		close(control[0]);
		snprintf(descriptor, sizeof(descriptor), "%d", wayland[1]);
		assert(setenv("WAYLAND_SOCKET", descriptor, 1) == 0);
		assert(setenv("FPLINUX_UI_SESSION_SOCKET", argv[2], 1) == 0);
		return repeat ? run_repeat_application(control[1]) :
				run_application(control[1]);
	}
	close(wayland[1]);
	close(control[1]);
	compositor.display = wl_display_create();
	assert(compositor.display);
	compositor.loop = wl_display_get_event_loop(compositor.display);
	assert(wl_display_init_shm(compositor.display) == 0);
	assert(wl_display_add_shm_format(compositor.display,
					 WL_SHM_FORMAT_RGB565));
	assert(wl_global_create(compositor.display, &wl_compositor_interface, 4,
				&compositor, bind_compositor));
	assert(wl_global_create(compositor.display, &xdg_wm_base_interface, 1,
				&compositor, bind_shell));
	if (repeat)
		add_keyboards(&compositor);
	assert(wl_client_create(compositor.display, wayland[0]));
	session = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	assert(session >= 0);
	wait_message(&compositor, session, FPLINUX_UI_SESSION_HELLO);
	strcpy(config.locale, "en");
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(session, &config));
	wait_message(&compositor, session, FPLINUX_UI_SESSION_READY);
	wait_commits(&compositor, 1);
	assert(compositor.colour == 0xf800 && compositor.uniform);
	if (!strcmp(argv[1], "disconnect")) {
		check_failed_exit(&compositor, session, control[0], child);
	} else {
		if (repeat)
			check_repeat(&compositor, session, control[0], argv[1]);
		else if (!strcmp(argv[1], "exit"))
			check_exit(&compositor, session, control[0], child);
		else if (!strcmp(argv[1], "softkeys"))
			check_softkeys(&compositor, session, control[0]);
		else
			check_ack(&compositor, session, control[0], argv[1]);
		finish_application(&compositor, session, child);
		if (repeat)
			check_repeat_closed(&compositor, control[0], argv[1]);
		close(session);
	}
	close(control[0]);
	close(listener);
	wl_display_destroy_clients(compositor.display);
	wl_display_destroy(compositor.display);
	assert(unlink(argv[2]) == 0);
	printf("%s: ok\n", argv[1]);
	return 0;
}
