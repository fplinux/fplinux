/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-core.h"
#include "session-wireless.h"
#include "launcher-observer.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct fixture {
	struct fplinux_session *session;
	struct fplinux_ui_frame_peer compositor;
	struct launcher_frame_observation frame;
	pid_t launcher;
	int observer;
	int audio_listener;
	int audio_control;
	bool hold_audio_ack;
	bool audio_ack_held;
	uint8_t held_audio_level;
	uint8_t audio_levels[16];
	unsigned int audio_count;
	char settings[PATH_MAX];
	char audio_socket[PATH_MAX];
	char session_socket[PATH_MAX];
	char applications[PATH_MAX];
	char clock_directory[PATH_MAX];
};

/* Wireless discovery is outside the volume/session boundary. */
struct fplinux_session_wireless *__wrap_fplinux_session_wireless_create(void)
{
	return NULL;
}

static void path_join(char *path, const char *root, const char *name)
{
	assert(snprintf(path, PATH_MAX, "%s/%s", root, name) < PATH_MAX);
}

static void write_file(const char *path, const char *contents)
{
	FILE *file = fopen(path, "w");

	assert(file && fputs(contents, file) >= 0);
	assert(fclose(file) == 0);
	assert(chmod(path, 0600) == 0);
}

static void acknowledge_volume(int fd, uint8_t class, uint8_t level)
{
	const uint8_t reply[] = { 8, class, level };

	assert(send(fd, reply, sizeof(reply), MSG_NOSIGNAL) == sizeof(reply));
}

/* The peer can hold the media acknowledgement while the Launcher preview changes. */
static void observe_audio(struct fixture *fixture)
{
	if (fixture->audio_control < 0) {
		fixture->audio_control = accept4(fixture->audio_listener, NULL,
						 NULL,
						 SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (fixture->audio_control < 0) {
			assert(errno == EAGAIN);
			return;
		}
	}
	for (;;) {
		uint8_t packet[10];
		ssize_t bytes = recv(fixture->audio_control, packet,
				     sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);

		if (bytes < 0 && errno == EAGAIN)
			return;
		assert(bytes > 0);
		if (packet[0] == 2) {
			assert(bytes == 2 && packet[1] <= 2);
			const uint8_t reply[] = { 9, packet[1] };

			assert(send(fixture->audio_control, reply,
				    sizeof(reply),
				    MSG_NOSIGNAL) == sizeof(reply));
			continue;
		}
		if (packet[0] != 1)
			continue;
		assert(bytes == 3 && (packet[1] == 1 || packet[1] == 2) &&
		       packet[2] <= 10);
		if (packet[1] == 2) {
			acknowledge_volume(fixture->audio_control, 2,
					   packet[2]);
			continue;
		}
		assert(fixture->audio_count < sizeof(fixture->audio_levels));
		fixture->audio_levels[fixture->audio_count++] = packet[2];
		if (fixture->hold_audio_ack) {
			assert(!fixture->audio_ack_held);
			fixture->held_audio_level = packet[2];
			fixture->audio_ack_held = true;
		} else {
			acknowledge_volume(fixture->audio_control, 1,
					   packet[2]);
		}
	}
}

static void pump(struct fixture *fixture)
{
	struct launcher_frame_observation frame;
	unsigned int commits = fixture->compositor.commits;
	ssize_t bytes;

	fplinux_ui_frame_pump(&fixture->compositor);
	fplinux_session_dispatch(fixture->session);
	if (fixture->compositor.commits != commits) {
		fplinux_session_surface_committed(fixture->session,
						  fixture->launcher, 240, 320);
		fplinux_session_frame(fixture->session, fixture->launcher);
	}
	observe_audio(fixture);
	fplinux_session_dispatch(fixture->session);
	while ((bytes = recv(fixture->observer, &frame, sizeof(frame),
			     MSG_DONTWAIT)) > 0) {
		assert(bytes == (ssize_t)sizeof(frame));
		if (!frame.timer_only)
			fixture->frame = frame;
		else {
			fixture->frame.volume_preview_serial =
				frame.volume_preview_serial;
			strcpy(fixture->frame.volume_preview_value,
			       frame.volume_preview_value);
		}
	}
}

static bool has_label(const struct fixture *fixture, const char *text)
{
	for (unsigned int i = 0; i < fixture->frame.label_count; ++i)
		if (!strcmp(fixture->frame.labels[i].text, text))
			return true;
	return false;
}

static void wait_label(struct fixture *fixture, const char *text, bool present)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (has_label(fixture, text) != present &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(fixture);
	if (has_label(fixture, text) != present) {
		fprintf(stderr, "Expected label %s present=%d; actual:", text,
			present);
		for (unsigned int i = 0; i < fixture->frame.label_count; ++i)
			fprintf(stderr, " [%s]", fixture->frame.labels[i].text);
		fputc('\n', stderr);
	}
	assert(has_label(fixture, text) == present);
}

static void press(struct fixture *fixture, unsigned int key)
{
	struct fplinux_ui_frame_keyboard *keyboard =
		&fixture->compositor.keyboards[1];

	fplinux_ui_frame_send_key(keyboard, key, true);
	fplinux_ui_frame_send_key(keyboard, key, false);
}

static void expect_settings(struct fixture *fixture, const char *volume)
{
	char actual[256], expected[256];
	FILE *file = fopen(fixture->settings, "r");
	size_t bytes;

	assert(file);
	bytes = fread(actual, 1, sizeof(actual) - 1, file);
	assert(bytes > 0 && !ferror(file) && fclose(file) == 0);
	actual[bytes] = '\0';
	assert(snprintf(expected, sizeof(expected),
			"screen_off_seconds=30\nauto_lock_seconds=off\n"
			"locale=en\ninput_locale=en\ntheme=light\n"
			"brightness=7\nvolume=%s\n",
			volume) < (int)sizeof(expected));
	if (strcmp(actual, expected))
		fprintf(stderr, "Expected settings:\n%sActual settings:\n%s",
			expected, actual);
	assert(!strcmp(actual, expected));
}

static void start(struct fixture *fixture, char **arguments)
{
	struct passwd *account = getpwuid(getuid());
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	char desktop[PATH_MAX * 2], path[PATH_MAX], descriptor[32];
	int wayland[2], observer[2];
	const char *root = arguments[3];

	assert(account);
	fixture->audio_control = -1;
	path_join(fixture->settings, root, "settings");
	path_join(fixture->audio_socket, root, "audio.sock");
	path_join(fixture->session_socket, root, "session.sock");
	path_join(fixture->applications, root, "applications");
	path_join(fixture->clock_directory, root, "clock");
	assert(mkdir(fixture->applications, 0700) == 0);
	assert(mkdir(fixture->clock_directory, 0700) == 0);
	path_join(path, fixture->applications, "org.fplinux.launcher.desktop");
	assert(snprintf(desktop, sizeof(desktop),
			"[Desktop Entry]\nType=Application\nName=Launcher\n"
			"Exec=%s\nX-FPLinux-Mode=launcher\n",
			arguments[1]) < (int)sizeof(desktop));
	write_file(path, desktop);
	write_file(
		fixture->settings,
		"screen_off_seconds=30\nauto_lock_seconds=off\n"
		"locale=en\ninput_locale=en\ntheme=light\nbrightness=7\nvolume=8\n");
	fixture->audio_listener = socket(
		AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	assert(fixture->audio_listener >= 0);
	assert(strlen(fixture->audio_socket) < sizeof(address.sun_path));
	strcpy(address.sun_path, fixture->audio_socket);
	assert(bind(fixture->audio_listener, (struct sockaddr *)&address,
		    sizeof(address)) == 0);
	assert(listen(fixture->audio_listener, 1) == 0);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	fixture->observer = observer[0];
	assert(fcntl(observer[0], F_SETFL, O_NONBLOCK) == 0);
	snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
	assert(setenv("FPLINUX_LAUNCHER_OBSERVER_FD", descriptor, 1) == 0);
	assert(setenv("FPLINUX_LAUNCHER_TEST_CLOCK", "1", 1) == 0);
	assert(setenv("FPLINUX_UI_DESKTOP_DIR", fixture->applications, 1) == 0);
	assert(setenv("XDG_STATE_HOME", root, 1) == 0);
	fplinux_ui_frame_client_environment(wayland[1],
					    fixture->session_socket);
	fplinux_ui_frame_init(&fixture->compositor, wayland[0], 240, 320,
			      arguments[2]);
	struct fplinux_session_options options = {
		.applications = fixture->applications,
		.socket_path = fixture->session_socket,
		.user = account->pw_name,
		.home = root,
		.app_uid = getuid(),
		.app_gid = getgid(),
		.metadata_uid = getuid(),
		.width = 240,
		.height = 320,
		.settings_path = fixture->settings,
		.clock_preferences_directory = fixture->clock_directory,
		.audio_socket = fixture->audio_socket,
		.audio_service = "/proc/self/exe",
		.audio_device = "/dev/null",
	};

	fixture->session = fplinux_session_create(&options);
	assert(fixture->session);
	fixture->launcher = fplinux_session_focused_pid(fixture->session);
	assert(fixture->launcher > 0);
	close(wayland[1]);
	close(observer[1]);
	wait_label(fixture, "Tasks", true);
	assert(fixture->compositor.keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&fixture->compositor.keyboards[1]);
	press(fixture, KEY_PICKUP_PHONE);
	wait_label(fixture, "Quick settings", true);
	wait_label(fixture, "8", true);
	press(fixture, KEY_DOWN);
	wait_label(fixture, "← 8 →", true);
	press(fixture, KEY_LEFT);
	wait_label(fixture, "Save", true);
	wait_label(fixture, "← 7 →", true);
	while (fixture->audio_count < 2)
		pump(fixture);
	assert(fixture->audio_levels[0] == 8);
	assert(fixture->audio_levels[1] == 7);
	uint64_t preview_deadline = fplinux_ui_frame_now_ms() + 3000;

	while ((!fixture->frame.volume_preview_serial ||
		strcmp(fixture->frame.volume_preview_value, "7")) &&
	       fplinux_ui_frame_now_ms() < preview_deadline)
		pump(fixture);
	assert(fixture->frame.volume_preview_serial);
	assert(!strcmp(fixture->frame.volume_preview_value, "7"));
}

int main(int argc, char **argv)
{
	struct fixture fixture = { 0 };
	uint64_t deadline;
	bool save;

	assert(argc == 5);
	assert(!strcmp(argv[4], "save") || !strcmp(argv[4], "right"));
	save = !strcmp(argv[4], "save");
	start(&fixture, argv);
	fixture.hold_audio_ack = true;
	assert(!fplinux_session_key(fixture.session, KEY_VOLUMEDOWN, true,
				    fplinux_ui_frame_now_ms()));
	assert(!fplinux_session_key(fixture.session, KEY_VOLUMEDOWN, false,
				    fplinux_ui_frame_now_ms()));
	deadline = fplinux_ui_frame_now_ms() + 1000;
	while (!fixture.audio_ack_held && fplinux_ui_frame_now_ms() < deadline)
		pump(&fixture);
	assert(fixture.audio_ack_held && fixture.held_audio_level == 6);
	press(&fixture, save ? KEY_F13 : KEY_RIGHT);
	wait_label(&fixture, save ? "Save" : "← 8 →", !save);
	pump(&fixture);
	acknowledge_volume(fixture.audio_control, 1, fixture.held_audio_level);
	fixture.audio_ack_held = false;
	fixture.hold_audio_ack = false;
	if (save) {
		wait_label(&fixture, "Back", true);
		assert(fixture.audio_count == 3);
		assert(fixture.audio_levels[2] == 6);
		expect_settings(&fixture, "6");
	} else {
		wait_label(&fixture, "← 7 →", true);
		assert(fixture.audio_count == 4);
		assert(fixture.audio_levels[3] == 7);
		wait_label(&fixture, "Cancel", true);
		expect_settings(&fixture, "8");
		press(&fixture, KEY_F14);
		wait_label(&fixture, "Back", true);
		wait_label(&fixture, "← 8 →", true);
		assert(fixture.audio_levels[fixture.audio_count - 1] == 8);
		expect_settings(&fixture, "8");
	}
	fplinux_session_destroy(fixture.session);
	close(fixture.audio_control);
	close(fixture.audio_listener);
	close(fixture.observer);
	fplinux_ui_frame_cleanup(&fixture.compositor);
	assert(unlink(fixture.audio_socket) == 0);
	printf("session-volume-%s: hardware and GUI intents survive a pending ACK\n",
	       argv[4]);
	return 0;
}
