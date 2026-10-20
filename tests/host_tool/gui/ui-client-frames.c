/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-ui-fonts.h"
#include "fplinux-ui-profile.h"
#include "fplinux-ui-screen.h"
#include "fplinux-ui-session.h"
#include "fplinux-ui.h"
#include "xdg-shell-server-protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>

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
};

struct application {
	int control;
	bool closed;
	lv_obj_t *softkeys;
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
	if (!compositor->held[0])
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

	assert(argc == 3);
	listener = listen_session(argv[2]);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, wayland) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, control) == 0);
	child = fork();
	assert(child >= 0);
	if (!child) {
		close(listener);
		close(wayland[0]);
		close(control[0]);
		snprintf(descriptor, sizeof(descriptor), "%d", wayland[1]);
		assert(setenv("WAYLAND_SOCKET", descriptor, 1) == 0);
		assert(setenv("FPLINUX_UI_SESSION_SOCKET", argv[2], 1) == 0);
		return run_application(control[1]);
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
		if (!strcmp(argv[1], "exit"))
			check_exit(&compositor, session, control[0], child);
		else if (!strcmp(argv[1], "softkeys"))
			check_softkeys(&compositor, session, control[0]);
		else
			check_ack(&compositor, session, control[0], argv[1]);
		finish_application(&compositor, session, child);
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
