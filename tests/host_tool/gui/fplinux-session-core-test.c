/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-core.h"
#include "settings-session-boundary.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fplinux-keypad.h"
#include "clock-wire.h"

#define TEST_PEERS 8
#define TEST_MESSAGES 64
#define TEST_LAUNCHER "org.fplinux.launcher"
#define TEST_APP "org.fplinux.notes"
#define TEST_SETTINGS "org.fplinux.settings"
#define TEST_CLOCK "org.fplinux.clock"

enum command_type {
	COMMAND_HELLO,
	COMMAND_CONNECT,
	COMMAND_SEND,
	COMMAND_SENT,
	COMMAND_EVENT,
	COMMAND_DISCONNECTED,
	COMMAND_HOLD_CLOSE,
	COMMAND_HOLD_ACTIVATION,
	COMMAND_CLOSE_DECISION,
	COMMAND_CRASH,
};

struct command {
	enum command_type type;
	pid_t pid;
	char app_id[FPLINUX_UI_SESSION_ID_BYTES];
	struct fplinux_ui_session_message message;
};

struct peer {
	int fd;
	pid_t pid;
	char id[FPLINUX_UI_SESSION_ID_BYTES];
	struct fplinux_ui_session_message messages[TEST_MESSAGES];
	size_t message_count;
	unsigned int sent;
	bool disconnected;
};

struct fixture {
	struct fplinux_session *session;
	struct peer peers[TEST_PEERS];
	size_t peer_count;
	int listener;
	pid_t brightness_pid;
	uint64_t now_ms;
	int width;
	int height;
	const char *locale;
	bool tearing_down;
	bool media;
	bool vibrator_on;
	bool fail_vibration;
	int audio_listener;
	int audio_control;
	char audio_socket[PATH_MAX];
	char audio_messages[128][256];
	size_t audio_message_count;
	unsigned int teardown_callbacks;
	bool frame_held;
	unsigned int visible_frame;
	unsigned int retained_frame;
	/* The controlled compositor's current composition and displayed pixels. */
	uint16_t scene_pixels[4];
	uint16_t presented_pixels[4];
	char root[PATH_MAX];
	char home[PATH_MAX];
	char applications[PATH_MAX];
	char socket_path[PATH_MAX];
	char peer_socket[PATH_MAX];
	char settings[PATH_MAX];
	char clock_preferences[PATH_MAX];
	char languages[PATH_MAX];
	char brightness_socket[PATH_MAX];
	char brightness_level[PATH_MAX];
	char brightness_effective[PATH_MAX];
	char leds[PATH_MAX];
	char keypad_brightness[PATH_MAX];
	char keypad_trigger[PATH_MAX];
	char power_supply[PATH_MAX];
	char battery_capacity[PATH_MAX];
	char charger_online[PATH_MAX];
};

static struct fixture *active_fixture;
static time_t current_time = 1735689600;
static bool clock_fails;
static bool sync_fails;
static bool directory_sync_fails;
static unsigned int sync_calls;
static pid_t peer_manager;

static void fail(const char *expression, int line)
{
	fprintf(stderr, "session core test line %d: %s (errno=%d)\n", line,
		expression, errno);
	exit(1);
}

#define CHECK(expression)                            \
	do {                                         \
		if (!(expression))                   \
			fail(#expression, __LINE__); \
	} while (0)

/* These wraps replace the privileged clock boundary, never the session code. */
int __wrap_clock_settime(clockid_t clock, const struct timespec *value)
{
	CHECK(clock == CLOCK_REALTIME);
	if (clock_fails) {
		errno = EPERM;
		return -1;
	}
	current_time = value->tv_sec;
	return 0;
}

time_t __wrap_time(time_t *destination)
{
	if (destination)
		*destination = current_time;
	return current_time;
}

int __real_fsync(int fd);

/* Persistence can fail while the applied preference remains live. */
int __wrap_fsync(int fd)
{
	struct stat status;

	++sync_calls;
	CHECK(fstat(fd, &status) == 0);
	if (sync_fails || (directory_sync_fails && S_ISDIR(status.st_mode))) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

ssize_t __real_write(int fd, const void *buffer, size_t size);

/* Regular fixture files replace the kernel's whole-attribute writes. */
ssize_t __wrap_write(int fd, const void *buffer, size_t size)
{
	struct fixture *fixture = active_fixture;
	char descriptor[64], path[PATH_MAX];
	ssize_t length;

	if (fixture && fixture->keypad_brightness[0]) {
		CHECK(snprintf(descriptor, sizeof(descriptor),
			       "/proc/self/fd/%d",
			       fd) < (int)sizeof(descriptor));
		length = readlink(descriptor, path, sizeof(path) - 1);
		if (length >= 0) {
			path[length] = '\0';
			if (!strcmp(path, fixture->keypad_brightness) ||
			    !strcmp(path, fixture->keypad_trigger)) {
				CHECK(ftruncate(fd, 0) == 0);
				CHECK(lseek(fd, 0, SEEK_SET) == 0);
			}
		}
	}
	return __real_write(fd, buffer, size);
}

static void cleanup(void)
{
	struct fixture *fixture = active_fixture;

	if (!fixture)
		return;
	fplinux_session_destroy(fixture->session);
	if (fixture->brightness_pid > 0) {
		kill(fixture->brightness_pid, SIGKILL);
		while (waitpid(fixture->brightness_pid, NULL, 0) < 0 &&
		       errno == EINTR)
			;
	}
	for (size_t i = 0; i < fixture->peer_count; ++i)
		close(fixture->peers[i].fd);
	if (fixture->listener >= 0)
		close(fixture->listener);
	if (fixture->audio_control >= 0)
		close(fixture->audio_control);
	if (fixture->audio_listener >= 0)
		close(fixture->audio_listener);
	free(fixture);
	active_fixture = NULL;
}

static void path_join(char *destination, const char *directory,
		      const char *name)
{
	CHECK(snprintf(destination, PATH_MAX, "%s/%s", directory, name) <
	      PATH_MAX);
}

static void write_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");

	CHECK(file);
	CHECK(fputs(text, file) >= 0);
	CHECK(fclose(file) == 0);
}

static int listen_at(const char *path)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	CHECK(fd >= 0);
	CHECK(strlen(path) < sizeof(address.sun_path));
	strcpy(address.sun_path, path);
	CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	CHECK(listen(fd, TEST_PEERS) == 0);
	return fd;
}

static int connect_at(const char *path)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	CHECK(fd >= 0);
	CHECK(strlen(path) < sizeof(address.sun_path));
	strcpy(address.sun_path, path);
	CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	return fd;
}

static void send_command(int fd, const struct command *command)
{
	CHECK(send(fd, command, sizeof(*command), MSG_NOSIGNAL) ==
	      (ssize_t)sizeof(*command));
}

/* A stubborn peer still honors the manager's parent-death signal. */
static void peer_termination(int signal_number)
{
	(void)signal_number;
	if (getppid() != peer_manager)
		_exit(0);
}

/* An executed UI peer uses the production protocol and a test control socket. */
static int run_peer(void)
{
	const char *id = getenv("FPLINUX_APP_ID");
	struct command command = { .type = COMMAND_HELLO, .pid = getpid() };
	int channel = connect_at(getenv("FPLINUX_TEST_PEER_SOCKET"));
	int session_fd = -1;
	bool hold_close = false;
	bool hold_activation = false;
	char close_decision[8] = "ready";

	peer_manager = getppid();
	CHECK(id && strlen(id) < sizeof(command.app_id));
	strcpy(command.app_id, id);
	send_command(channel, &command);
	for (;;) {
		struct pollfd fds[] = {
			{ .fd = channel, .events = POLLIN },
			{ .fd = session_fd, .events = POLLIN },
		};

		int result = poll(fds, 2, -1);

		if (result < 0 && errno == EINTR)
			continue;
		CHECK(result > 0);
		if (fds[0].revents & (POLLIN | POLLHUP)) {
			ssize_t bytes =
				recv(channel, &command, sizeof(command), 0);

			if (bytes <= 0)
				return 0;
			CHECK(bytes == (ssize_t)sizeof(command));
			switch (command.type) {
			case COMMAND_CONNECT:
				if (session_fd >= 0)
					close(session_fd);
				session_fd = fplinux_ui_session_connect(
					command.app_id[0] ? command.app_id :
							    id);
				CHECK(session_fd >= 0);
				break;
			case COMMAND_SEND:
				CHECK(fplinux_ui_session_send(
					session_fd, &command.message));
				command.type = COMMAND_SENT;
				send_command(channel, &command);
				break;
			case COMMAND_HOLD_CLOSE:
				hold_close = true;
				CHECK(signal(SIGTERM, peer_termination) !=
				      SIG_ERR);
				command.type = COMMAND_SENT;
				send_command(channel, &command);
				break;
			case COMMAND_HOLD_ACTIVATION:
				hold_activation = command.message.enabled;
				command.type = COMMAND_SENT;
				send_command(channel, &command);
				break;
			case COMMAND_CLOSE_DECISION:
				CHECK(strlen(command.message.value) <
				      sizeof(close_decision));
				strcpy(close_decision, command.message.value);
				command.type = COMMAND_SENT;
				send_command(channel, &command);
				break;
			case COMMAND_CRASH:
				return 23;
			default:
				CHECK(false);
			}
		}
		if (session_fd < 0 || !(fds[1].revents & (POLLIN | POLLHUP)))
			continue;
		for (;;) {
			int received = fplinux_ui_session_receive(
				session_fd, &command.message);

			if (!received)
				break;
			if (received < 0) {
				command.type = COMMAND_DISCONNECTED;
				send_command(channel, &command);
				close(session_fd);
				session_fd = -1;
				break;
			}
			command.type = COMMAND_EVENT;
			send_command(channel, &command);
			/* This peer replaces the client frame boundary, tested separately. */
			if (command.message.type ==
				    FPLINUX_UI_SESSION_ACTIVATION &&
			    !hold_activation) {
				struct fplinux_ui_session_message response = {
					.type = FPLINUX_UI_SESSION_ACTIVATION_ACK,
					.serial = command.message.serial,
				};

				CHECK(fplinux_ui_session_send(session_fd,
							      &response));
			}
			if (command.message.type ==
				    FPLINUX_UI_SESSION_PREPARE_CLOSE &&
			    close_decision[0]) {
				struct fplinux_ui_session_message response = {
					.type = FPLINUX_UI_SESSION_CLOSE_REPLY,
					.serial = command.message.serial,
				};

				strcpy(response.value, close_decision);
				CHECK(fplinux_ui_session_send(session_fd,
							      &response));
			}
			if (command.message.type == FPLINUX_UI_SESSION_CLOSE &&
			    !hold_close) {
				char path[PATH_MAX];
				struct fplinux_ui_session_message exit = {
					.type = FPLINUX_UI_SESSION_EXIT,
				};

				path_join(path, getenv("HOME"), id);
				write_file(path, "saved\n");
				CHECK(fplinux_ui_session_send(session_fd,
							      &exit));
				close(session_fd);
				close(channel);
				return 0;
			}
		}
	}
}

/* A wire-level service keeps SET separate from the display's leased SHOW. */
static void run_brightness_service(int listener, const char *level_path,
				   const char *effective_path)
{
	unsigned int level = 7;
	unsigned int effective = 7;
	bool leased = false;
	int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

	CHECK(client >= 0);
	for (;;) {
		char request[32], response[32];
		unsigned int next;
		ssize_t bytes = recv(client, request, sizeof(request) - 1, 0);

		if (bytes <= 0)
			_exit(0);
		request[bytes] = '\0';
		if (!strcmp(request, "GET")) {
			snprintf(response, sizeof(response), "LEVEL %u", level);
		} else {
			if (sscanf(request, "SET %u", &next) == 1) {
				char stored[16];

				CHECK(next >= 1 && next <= 10);
				level = next;
				if (!leased)
					effective = level;
				snprintf(stored, sizeof(stored), "%u\n", level);
				write_file(level_path, stored);
			} else {
				if (!strcmp(request, "CLAIM")) {
					leased = true;
				} else if (!strcmp(request, "RELEASE")) {
					CHECK(leased);
					leased = false;
					effective = level;
				} else {
					CHECK(sscanf(request, "SHOW %u",
						     &next) == 1);
					CHECK(leased && next <= 10);
					effective = next;
				}
			}
			char stored[16];

			snprintf(stored, sizeof(stored), "%u\n", effective);
			write_file(effective_path, stored);
			strcpy(response, "OK");
		}
		CHECK(send(client, response, strlen(response), MSG_NOSIGNAL) ==
		      (ssize_t)strlen(response));
	}
}

static uint64_t clock_ms(void *context)
{
	return ((struct fixture *)context)->now_ms;
}

static void session_changed(void *context)
{
	struct fixture *fixture = context;

	if (fixture->tearing_down)
		++fixture->teardown_callbacks;
}

static void hold_frame(void *context, bool enabled)
{
	struct fixture *fixture = context;

	fixture->frame_held = enabled;
	if (enabled) {
		fixture->retained_frame = fixture->visible_frame;
	} else {
		memcpy(fixture->presented_pixels, fixture->scene_pixels,
		       sizeof(fixture->presented_pixels));
	}
}

static void present_scene(struct fixture *fixture, const uint16_t pixels[4])
{
	memcpy(fixture->scene_pixels, pixels, sizeof(fixture->scene_pixels));
	if (!fixture->frame_held)
		memcpy(fixture->presented_pixels, pixels,
		       sizeof(fixture->presented_pixels));
}

static void expect_scene(struct fixture *fixture, const uint16_t pixels[4])
{
	CHECK(!memcmp(fixture->presented_pixels, pixels,
		      sizeof(fixture->presented_pixels)));
}

static void add_entry(struct fixture *fixture, const char *id, const char *mode)
{
	char path[PATH_MAX], name[160], executable[PATH_MAX];
	FILE *file;
	ssize_t bytes =
		readlink("/proc/self/exe", executable, sizeof(executable) - 1);

	CHECK(bytes > 0);
	executable[bytes] = '\0';
	CHECK(snprintf(name, sizeof(name), "%s.desktop", id) <
	      (int)sizeof(name));
	path_join(path, fixture->applications, name);
	file = fopen(path, "w");
	CHECK(file);
	CHECK(fprintf(file,
		      "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n"
		      "X-FPLinux-Mode=%s\n",
		      id, executable, mode) > 0);
	CHECK(fclose(file) == 0);
	CHECK(chmod(path, 0600) == 0);
}

/* The external audio service records policy packets; it performs no mixing. */
static void observe_audio(struct fixture *fixture)
{
	if (fixture->audio_listener < 0)
		return;
	if (fixture->audio_control < 0) {
		fixture->audio_control = accept4(fixture->audio_listener, NULL,
						 NULL,
						 SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (fixture->audio_control < 0)
			return;
	}
	for (;;) {
		char packet[256];
		ssize_t bytes = recv(fixture->audio_control, packet,
				     sizeof(packet) - 1, MSG_DONTWAIT);

		if (bytes < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		if (!bytes) {
			close(fixture->audio_control);
			fixture->audio_control = -1;
			return;
		}
		CHECK(bytes > 0 && fixture->audio_message_count < 128);
		packet[bytes] = '\0';
		strcpy(fixture->audio_messages[fixture->audio_message_count++],
		       packet);
	}
}

static bool test_vibrate(void *context, bool on)
{
	struct fixture *fixture = context;

	CHECK(fixture->vibrator_on != on);
	if (fixture->fail_vibration) {
		fixture->fail_vibration = false;
		errno = EIO;
		return false;
	}
	fixture->vibrator_on = on;
	return true;
}

static struct fixture *create_fixture(const char *root, int width, int height,
				      bool brightness, unsigned int keypad_max,
				      const char *locale, int battery_percent,
				      bool media)
{
	struct fixture *fixture = calloc(1, sizeof(*fixture));
	struct passwd *account = getpwuid(getuid());
	struct fplinux_session_options options;
	char path[PATH_MAX], settings[256];

	CHECK(fixture && account);
	active_fixture = fixture;
	fixture->listener = -1;
	fixture->audio_listener = -1;
	fixture->audio_control = -1;
	fixture->media = media;
	fixture->now_ms = 1000;
	fixture->width = width;
	fixture->height = height;
	fixture->locale = locale;
	CHECK(strlen(root) < sizeof(fixture->root));
	strcpy(fixture->root, root);
	path_join(fixture->home, root, "home");
	path_join(fixture->applications, root, "apps");
	path_join(fixture->socket_path, root, "session.sock");
	path_join(fixture->peer_socket, root, "peer.sock");
	path_join(fixture->settings, root, "settings");
	path_join(fixture->languages, root, "languages");
	path_join(fixture->brightness_socket, root, "brightness.sock");
	path_join(fixture->brightness_level, root, "brightness-level");
	path_join(fixture->brightness_effective, root, "brightness-effective");
	CHECK(mkdir(fixture->home, 0700) == 0);
	CHECK(mkdir(fixture->applications, 0700) == 0);
	CHECK(mkdir(fixture->languages, 0700) == 0);
	path_join(path, fixture->languages, "ru");
	if (!strcmp(locale, "ru")) {
		char source[PATH_MAX];

		path_join(source, root, "system-language");
		CHECK(rename(source, path) == 0);
	} else {
		write_file(
			path,
			"name=Russian\nsystem=no\ninput=yes\n"
			"input.indicator.sentence=Абв\ninput.indicator.lower=абв\n"
			"input.indicator.upper=АБВ\n");
	}
	CHECK(snprintf(settings, sizeof(settings),
		       "screen_off_seconds=30\nauto_lock_seconds=%s\n"
		       "locale=%s\ninput_locale=en\ntheme=light\nbrightness=7\n",
		       media ? "0" : "off", locale) < (int)sizeof(settings));
	write_file(fixture->settings, settings);
	fixture->listener = listen_at(fixture->peer_socket);
	CHECK(fcntl(fixture->listener, F_SETFL, O_NONBLOCK) == 0);
	CHECK(setenv("FPLINUX_TEST_PEER_SOCKET", fixture->peer_socket, 1) == 0);
	CHECK(setenv("TZ", "UTC", 1) == 0);
	tzset();
	add_entry(fixture, TEST_LAUNCHER, "launcher");
	add_entry(fixture, TEST_APP, "integrated");
	add_entry(fixture, TEST_SETTINGS, "integrated");
	add_entry(fixture, TEST_CLOCK, "integrated");
	if (brightness) {
		int listener = listen_at(fixture->brightness_socket);

		write_file(fixture->brightness_level, "7\n");
		write_file(fixture->brightness_effective, "7\n");
		fixture->brightness_pid = fork();
		CHECK(fixture->brightness_pid >= 0);
		if (!fixture->brightness_pid)
			run_brightness_service(listener,
					       fixture->brightness_level,
					       fixture->brightness_effective);
		close(listener);
	}
	if (keypad_max) {
		char light[PATH_MAX];

		path_join(fixture->leds, root, "leds");
		CHECK(mkdir(fixture->leds, 0700) == 0);
		path_join(light, fixture->leds, "platform:kbd_backlight");
		CHECK(mkdir(light, 0700) == 0);
		path_join(fixture->keypad_brightness, light, "brightness");
		path_join(fixture->keypad_trigger, light, "trigger");
		write_file(fixture->keypad_brightness,
			   keypad_max == 1 ? "1\n" : "37\n");
		write_file(fixture->keypad_trigger,
			   "none [timer] input-events\n");
		path_join(path, light, "max_brightness");
		write_file(path, keypad_max == 1 ? "1\n" : "255\n");
	}
	if (battery_percent >= 0) {
		char supply[PATH_MAX], capacity[16];

		path_join(fixture->power_supply, root, "power-supply");
		CHECK(mkdir(fixture->power_supply, 0700) == 0);
		path_join(supply, fixture->power_supply, "battery");
		CHECK(mkdir(supply, 0700) == 0);
		path_join(path, supply, "type");
		write_file(path, "Battery\n");
		path_join(fixture->battery_capacity, supply, "capacity");
		CHECK(snprintf(capacity, sizeof(capacity), "%d\n",
			       battery_percent) < (int)sizeof(capacity));
		write_file(fixture->battery_capacity, capacity);
		path_join(supply, fixture->power_supply, "charger");
		CHECK(mkdir(supply, 0700) == 0);
		path_join(path, supply, "type");
		write_file(path, "Unknown\n");
		path_join(fixture->charger_online, supply, "online");
		write_file(fixture->charger_online, "0\n");
	}
	path_join(fixture->clock_preferences, root, "clocks");
	if (media) {
		path_join(fixture->audio_socket, root, "audio.sock");
		fixture->audio_listener = listen_at(fixture->audio_socket);
		CHECK(fcntl(fixture->audio_listener, F_SETFL, O_NONBLOCK) == 0);
	}
	options = (struct fplinux_session_options){
		.applications = fixture->applications,
		.socket_path = fixture->socket_path,
		.user = account->pw_name,
		.home = fixture->home,
		.app_uid = getuid(),
		.app_gid = getgid(),
		.metadata_uid = getuid(),
		.width = width,
		.height = height,
		.settings_path = fixture->settings,
		.clock_preferences_directory = fixture->clock_preferences,
		.languages_dir = fixture->languages,
		.brightness_socket = brightness ? fixture->brightness_socket :
						  NULL,
		.leds_dir = keypad_max ? fixture->leds : NULL,
		.power_supply_dir =
			battery_percent >= 0 ? fixture->power_supply : NULL,
		.clock_signal_path = getenv("FPLINUX_TEST_CLOCK_SIGNAL_FILE") ?
					     "/proc/self/exe" :
					     NULL,
		.clock_ms = clock_ms,
		.audio_socket = media ? fixture->audio_socket : NULL,
		.audio_service = media ? "/proc/self/exe" : NULL,
		.audio_device = media ? "/dev/null" : NULL,
		.vibrate = media ? test_vibrate : NULL,
		.changed = session_changed,
		.hold_frame = hold_frame,
		.context = fixture,
	};
	fixture->session = fplinux_session_create(&options);
	CHECK(fixture->session);
	return fixture;
}

static uint64_t real_ms(void)
{
	struct timespec time;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
	return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static void observe_peer(struct peer *peer)
{
	struct command event;
	ssize_t bytes;

	while ((bytes = recv(peer->fd, &event, sizeof(event), 0)) > 0) {
		CHECK(bytes == (ssize_t)sizeof(event));
		if (event.type == COMMAND_SENT)
			++peer->sent;
		else if (event.type == COMMAND_DISCONNECTED)
			peer->disconnected = true;
		else {
			CHECK(event.type == COMMAND_EVENT);
			CHECK(peer->message_count < TEST_MESSAGES);
			peer->messages[peer->message_count++] = event.message;
		}
	}
}

static void pump(struct fixture *fixture, unsigned int duration_ms)
{
	uint64_t deadline = real_ms() + duration_ms;

	do {
		struct pollfd fd = {
			.fd = fplinux_session_get_fd(fixture->session),
			.events = POLLIN,
		};
		int channel;

		fplinux_session_dispatch(fixture->session);
		observe_audio(fixture);
		while ((channel = accept4(fixture->listener, NULL, NULL,
					  SOCK_CLOEXEC)) >= 0) {
			struct command hello;
			struct pollfd peer_fd = { .fd = channel,
						  .events = POLLIN };
			struct peer *peer;

			CHECK(fixture->peer_count < TEST_PEERS);
			CHECK(poll(&peer_fd, 1, 1000) == 1);
			CHECK(recv(channel, &hello, sizeof(hello), 0) ==
			      (ssize_t)sizeof(hello));
			CHECK(hello.type == COMMAND_HELLO);
			peer = &fixture->peers[fixture->peer_count++];
			peer->pid = hello.pid;
			peer->fd = channel;
			strcpy(peer->id, hello.app_id);
			CHECK(fcntl(channel, F_SETFL, O_NONBLOCK) == 0);
		}
		for (size_t i = 0; i < fixture->peer_count; ++i)
			observe_peer(&fixture->peers[i]);
		poll(&fd, 1, 2);
	} while (real_ms() < deadline);
}

static struct peer *wait_peer(struct fixture *fixture, const char *id)
{
	uint64_t deadline = real_ms() + 2000;

	do {
		pump(fixture, 2);
		for (size_t i = 0; i < fixture->peer_count; ++i)
			if (!strcmp(fixture->peers[i].id, id) &&
			    fplinux_session_find_process(fixture->session,
							 fixture->peers[i].pid))
				return &fixture->peers[i];
	} while (real_ms() < deadline);
	CHECK(false);
	return NULL;
}

static bool take_message(struct peer *peer,
			 enum fplinux_ui_session_message_type type,
			 const char *name,
			 struct fplinux_ui_session_message *message)
{
	for (size_t i = 0; i < peer->message_count; ++i) {
		if (peer->messages[i].type != type ||
		    (name && strcmp(peer->messages[i].name, name)))
			continue;
		*message = peer->messages[i];
		--peer->message_count;
		memmove(&peer->messages[i], &peer->messages[i + 1],
			(peer->message_count - i) * sizeof(*message));
		return true;
	}
	return false;
}

static struct fplinux_ui_session_message
wait_message(struct fixture *fixture, struct peer *peer,
	     enum fplinux_ui_session_message_type type, const char *name)
{
	struct fplinux_ui_session_message message;
	uint64_t deadline = real_ms() + 2000;

	do {
		if (take_message(peer, type, name, &message))
			return message;
		pump(fixture, 2);
	} while (real_ms() < deadline);
	fprintf(stderr, "waiting for message type=%d name=%s peer=%s\n", type,
		name ? name : "", peer->id);
	CHECK(false);
	return (struct fplinux_ui_session_message){ 0 };
}

static void issue(struct fixture *fixture, struct peer *peer,
		  const struct fplinux_ui_session_message *message)
{
	struct command command = { .type = COMMAND_SEND, .message = *message };
	unsigned int sent = peer->sent;
	uint64_t deadline = real_ms() + 2000;

	send_command(peer->fd, &command);
	/* Dispatch can reject the message and terminate its sender. */
	while (peer->sent == sent && real_ms() < deadline) {
		struct pollfd fd = { .fd = peer->fd, .events = POLLIN };

		poll(&fd, 1, 2);
		observe_peer(peer);
	}
	CHECK(peer->sent > sent);
	pump(fixture, 4);
}

static void send_type(struct fixture *fixture, struct peer *peer,
		      enum fplinux_ui_session_message_type type)
{
	struct fplinux_ui_session_message message = { .type = type };

	issue(fixture, peer, &message);
}

static struct fplinux_ui_session_message
authenticate(struct fixture *fixture, struct peer *peer, const char *id)
{
	struct command command = { .type = COMMAND_CONNECT };

	if (id)
		strcpy(command.app_id, id);
	send_command(peer->fd, &command);
	return wait_message(fixture, peer, FPLINUX_UI_SESSION_CONFIG, NULL);
}

static struct fplinux_ui_session_message
active_for(struct fixture *fixture, struct peer *launcher, const char *id)
{
	struct fplinux_ui_session_message message;

	do {
		message = wait_message(fixture, launcher,
				       FPLINUX_UI_SESSION_ACTIVE, NULL);
	} while (strcmp(message.active_id, id));
	return message;
}

static void acknowledge(struct fixture *fixture, struct peer *launcher,
			const struct fplinux_ui_session_message *active)
{
	struct fplinux_ui_session_message ack = {
		.type = FPLINUX_UI_SESSION_ACTIVE_ACK,
		.serial = active->serial,
	};

	CHECK(active->serial != 0);
	issue(fixture, launcher, &ack);
}

static struct peer *start_launcher(struct fixture *fixture)
{
	struct peer *launcher = wait_peer(fixture, TEST_LAUNCHER);
	struct fplinux_ui_session_message config =
		authenticate(fixture, launcher, NULL);

	CHECK(config.mode == FPLINUX_UI_SESSION_LAUNCHER);
	CHECK(!config.audio && !config.vibration);
	CHECK(config.screen_width == fixture->width &&
	      config.screen_height == fixture->height);
	CHECK(!strcmp(config.locale, fixture->locale) &&
	      !strcmp(config.input_locale, "en"));
	CHECK(!fplinux_session_is_ready(fixture->session));
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	CHECK(!fplinux_session_is_ready(fixture->session));
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	CHECK(!fplinux_session_is_ready(fixture->session));
	fplinux_session_frame(fixture->session, launcher->pid);
	CHECK(fplinux_session_is_ready(fixture->session));
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	acknowledge(fixture, launcher, &active);
	return launcher;
}

static void test_startup_frame_before_ready(struct fixture *fixture)
{
	struct peer *launcher = wait_peer(fixture, TEST_LAUNCHER);

	authenticate(fixture, launcher, NULL);
	CHECK(!fplinux_session_is_ready(fixture->session));
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	fplinux_session_frame(fixture->session, launcher->pid);
	CHECK(!fplinux_session_is_ready(fixture->session));
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	CHECK(fplinux_session_is_ready(fixture->session));
}

static struct peer *launch_app(struct fixture *fixture, struct peer *launcher,
			       const char *id)
{
	struct fplinux_ui_session_message launch = {
		.type = FPLINUX_UI_SESSION_LAUNCH
	};
	pid_t focused = fplinux_session_focused_pid(fixture->session);
	const struct fplinux_session_process *current =
		fplinux_session_find_process(fixture->session, focused);
	bool replaces_app = current && focused != launcher->pid &&
			    strcmp(current->app_id, id);

	strcpy(launch.app_id, id);
	issue(fixture, launcher, &launch);
	if (replaces_app) {
		struct fplinux_ui_session_message active =
			active_for(fixture, launcher, "");

		acknowledge(fixture, launcher, &active);
	}
	return wait_peer(fixture, id);
}

static void commit_app(struct fixture *fixture, struct peer *app,
		       const struct fplinux_ui_session_message *config)
{
	int x, y, width, height;

	fplinux_session_surface_box(fixture->session, config->mode, &x, &y,
				    &width, &height);
	send_type(fixture, app, FPLINUX_UI_SESSION_READY);
	fplinux_session_frame(fixture->session, app->pid);
	fplinux_session_surface_committed(fixture->session, app->pid, width,
					  height);
}

static struct peer *show_app(struct fixture *fixture, struct peer *launcher,
			     const char *id)
{
	struct peer *app = launch_app(fixture, launcher, id);
	struct fplinux_ui_session_message config =
		authenticate(fixture, app, NULL);
	if (fixture->media)
		CHECK(config.audio && config.vibration);
	commit_app(fixture, app, &config);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, id);

	acknowledge(fixture, launcher, &active);
	CHECK(fplinux_session_find_process(fixture->session, app->pid)->shown);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	return app;
}

static void clear_messages(struct fixture *fixture, struct peer *peer)
{
	pump(fixture, 20);
	peer->message_count = 0;
}

static void wait_reaped(struct fixture *fixture, pid_t pid)
{
	uint64_t deadline = real_ms() + 2000;

	while (fplinux_session_find_process(fixture->session, pid) &&
	       real_ms() < deadline)
		pump(fixture, 2);
	CHECK(!fplinux_session_find_process(fixture->session, pid));
	errno = 0;
	CHECK(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
}

static void expect_file(const char *path, const char *expected)
{
	char text[512];
	FILE *file = fopen(path, "r");
	size_t bytes;

	CHECK(file);
	bytes = fread(text, 1, sizeof(text) - 1, file);
	CHECK(!ferror(file));
	text[bytes] = '\0';
	CHECK(fclose(file) == 0);
	CHECK(!strcmp(text, expected));
}

static void expect_number(const char *path, int expected)
{
	FILE *file = fopen(path, "r");
	int value;
	char trailing;

	CHECK(file);
	CHECK(fscanf(file, "%d", &value) == 1);
	CHECK(value == expected);
	CHECK(fscanf(file, " %c", &trailing) == EOF);
	CHECK(!ferror(file));
	CHECK(fclose(file) == 0);
}

static void test_home_retains_exit_saves(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	pid_t pid = app->pid;
	char saved[PATH_MAX];

	clear_messages(fixture, launcher);
	clear_messages(fixture, app);
	fplinux_session_home(fixture->session);
	struct fplinux_ui_session_message home =
		active_for(fixture, launcher, "");

	CHECK(!strcmp(home.value, "home"));
	acknowledge(fixture, launcher, &home);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, pid);

	CHECK(process && process->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	CHECK(!process->shown && !process->closing);
	struct fplinux_ui_session_message ignored;

	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	path_join(saved, fixture->home, TEST_APP);
	CHECK(access(saved, F_OK) == -1 && errno == ENOENT);
	CHECK(launch_app(fixture, launcher, TEST_APP)->pid == pid);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_APP);

	acknowledge(fixture, launcher, &active);
	process = fplinux_session_find_process(fixture->session, pid);
	CHECK(process && process->shown);
	clear_messages(fixture, launcher);
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	active = active_for(fixture, launcher, "");
	CHECK(!strcmp(active.value, "catalogue"));
	CHECK(!strcmp(active.selected_id, TEST_APP));
	acknowledge(fixture, launcher, &active);
	wait_message(fixture, app, FPLINUX_UI_SESSION_CLOSE, NULL);
	wait_reaped(fixture, pid);
	expect_file(saved, "saved\n");
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
}

static void set_close_decision(struct fixture *fixture, struct peer *peer,
			       const char *decision)
{
	struct command command = { .type = COMMAND_CLOSE_DECISION };
	unsigned int sent = peer->sent;
	uint64_t deadline = real_ms() + 2000;

	strcpy(command.message.value, decision);
	send_command(peer->fd, &command);
	while (peer->sent == sent && real_ms() < deadline)
		pump(fixture, 2);
	CHECK(peer->sent > sent);
}

static void answer_close(struct fixture *fixture, struct peer *peer,
			 uint32_t serial, const char *decision)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE_REPLY,
		.serial = serial,
	};

	strcpy(message.value, decision);
	issue(fixture, peer, &message);
}

static void acknowledge_close(struct fixture *fixture, struct peer *peer,
			      uint32_t serial)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE_ACK,
		.serial = serial,
	};

	issue(fixture, peer, &message);
}

static void return_home(struct fixture *fixture, struct peer *launcher);
static struct fplinux_ui_session_message query_tasks(struct fixture *fixture,
						     struct peer *launcher);
static const struct fplinux_ui_session_task *
task_record(const struct fplinux_ui_session_message *snapshot, const char *id);
static void task_action(struct fixture *fixture, struct peer *launcher,
			enum fplinux_ui_session_message_type type,
			const char *id, uint32_t token);

static void test_close_cancel_and_stale_reply(struct fixture *fixture)
{
	const uint16_t initial[] = { 0xf800, 0xf800, 0x0000, 0xffff };
	const uint16_t restored[] = { 0x001f, 0x001f, 0xffff, 0x0000 };
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message ignored;

	present_scene(fixture, initial);
	set_close_decision(fixture, app, "");
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	struct fplinux_ui_session_message first = wait_message(
		fixture, app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);

	CHECK(first.serial);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, app->pid);

	CHECK(process && !process->closing && process->shown);
	CHECK(process->lifecycle == FPLINUX_UI_SESSION_FOREGROUND);
	present_scene(fixture, restored);
	expect_scene(fixture, initial);
	answer_close(fixture, app, first.serial, "cancel");
	expect_scene(fixture, initial);
	acknowledge_close(fixture, app, first.serial);
	expect_scene(fixture, restored);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	struct fplinux_ui_session_message second = wait_message(
		fixture, app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);

	CHECK(second.serial && second.serial != first.serial);
	answer_close(fixture, app, first.serial, "ready");
	process = fplinux_session_find_process(fixture->session, app->pid);
	CHECK(process && !process->closing && process->shown);
	answer_close(fixture, app, second.serial, "ready");
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	CHECK(fixture->frame_held);
	acknowledge(fixture, launcher, &active);
	wait_message(fixture, app, FPLINUX_UI_SESSION_CLOSE, NULL);
	wait_reaped(fixture, app->pid);
}

static void test_close_ready_retains_scene_before_reply(struct fixture *fixture)
{
	const uint16_t covering[] = { 0xaaaa, 0xffff, 0xffff, 0xaaaa };
	const uint16_t application[] = { 0xf800, 0x07e0, 0x001f, 0xffff };
	const uint16_t destination[] = { 0x1111, 0x1111, 0x2222, 0x2222 };
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);

	present_scene(fixture, covering);
	set_close_decision(fixture, app, "");
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	struct fplinux_ui_session_message prepare = wait_message(
		fixture, app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);

	/* Removing a covering view cannot publish the application's old frame. */
	present_scene(fixture, application);
	expect_scene(fixture, covering);
	fixture->now_ms += 1000;
	pump(fixture, 4);
	expect_scene(fixture, covering);
	answer_close(fixture, app, prepare.serial, "ready");
	expect_scene(fixture, covering);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	present_scene(fixture, destination);
	expect_scene(fixture, covering);
	acknowledge(fixture, launcher, &active);
	expect_scene(fixture, destination);
	wait_message(fixture, app, FPLINUX_UI_SESSION_CLOSE, NULL);
	wait_reaped(fixture, app->pid);
}

static void
test_close_wait_needs_decision_and_system_frames(struct fixture *fixture)
{
	const uint16_t initial[] = { 0xaaaa, 0xffff, 0xffff, 0xaaaa };
	const uint16_t decision[] = { 0x0000, 0xffe0, 0xffe0, 0x0000 };
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message legends = {
		.type = FPLINUX_UI_SESSION_SOFTKEYS,
		.left = "Leave",
		.right = "Stay",
	};

	present_scene(fixture, initial);
	set_close_decision(fixture, app, "");
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	struct fplinux_ui_session_message prepare = wait_message(
		fixture, app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);

	answer_close(fixture, app, prepare.serial, "wait");
	issue(fixture, app, &legends);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_APP);

	present_scene(fixture, decision);
	expect_scene(fixture, initial);
	acknowledge_close(fixture, app, prepare.serial + 1);
	expect_scene(fixture, initial);
	acknowledge_close(fixture, app, prepare.serial);
	expect_scene(fixture, initial);
	acknowledge(fixture, launcher, &active);
	expect_scene(fixture, decision);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
}

static void
test_close_wait_survives_home_and_repeated_close(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message ignored;

	set_close_decision(fixture, app, "wait");
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	struct fplinux_ui_session_message prepare = wait_message(
		fixture, app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);
	pump(fixture, 10);
	fixture->now_ms += 4000;
	pump(fixture, 10);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, app->pid);

	CHECK(process && !process->closing && process->shown);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	acknowledge_close(fixture, app, prepare.serial);
	return_home(fixture, launcher);
	struct fplinux_ui_session_message tasks =
		query_tasks(fixture, launcher);
	uint32_t token = task_record(&tasks, TEST_APP)->token;

	task_action(fixture, launcher, FPLINUX_UI_SESSION_CLOSE_TASK, TEST_APP,
		    token);
	struct fplinux_ui_session_message repeated = wait_message(
		fixture, app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_APP);

	CHECK(repeated.serial == prepare.serial);
	acknowledge(fixture, launcher, &active);
	acknowledge_close(fixture, app, prepare.serial);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	CHECK(!fplinux_session_find_process(fixture->session, app->pid)
		       ->closing);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	CHECK(fixture->peer_count == 2);
}

static void test_close_unanswered_has_bounded_timeout(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);

	set_close_decision(fixture, app, "");
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	wait_message(fixture, app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);
	fixture->now_ms += 2999;
	pump(fixture, 4);
	CHECK(!fplinux_session_find_process(fixture->session, app->pid)
		       ->closing);
	++fixture->now_ms;
	pump(fixture, 4);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	acknowledge(fixture, launcher, &active);
	wait_message(fixture, app, FPLINUX_UI_SESSION_CLOSE, NULL);
	wait_reaped(fixture, app->pid);
}

static void
test_delayed_close_wait_after_home_stays_hidden(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);

	set_close_decision(fixture, app, "");
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	struct fplinux_ui_session_message prepare = wait_message(
		fixture, app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);

	return_home(fixture, launcher);
	answer_close(fixture, app, prepare.serial, "wait");
	pump(fixture, 10);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, app->pid);

	CHECK(process && !process->closing && !process->shown);
	CHECK(process->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
	CHECK(!fixture->frame_held);
}

static void
test_shutdown_saves_and_bounds_unresponsive_peers(struct fixture *fixture)
{
	const char *editor_id = "org.example.editor";
	struct peer *launcher = start_launcher(fixture);
	struct peer *parked = show_app(fixture, launcher, TEST_APP);
	struct peer *stubborn = show_app(fixture, launcher, TEST_SETTINGS);
	struct command hold = { .type = COMMAND_HOLD_CLOSE };
	char parked_saved[PATH_MAX], foreground_saved[PATH_MAX];
	unsigned int sent = stubborn->sent;
	uint64_t deadline = real_ms() + 2000;

	send_command(stubborn->fd, &hold);
	while (stubborn->sent == sent && real_ms() < deadline)
		pump(fixture, 2);
	CHECK(stubborn->sent > sent);
	add_entry(fixture, editor_id, "integrated");
	struct peer *foreground = show_app(fixture, launcher, editor_id);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, parked->pid);

	CHECK(process && process->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	CHECK(!process->shown && !process->closing);
	CHECK(fplinux_session_focused_pid(fixture->session) == foreground->pid);
	path_join(parked_saved, fixture->home, TEST_APP);
	path_join(foreground_saved, fixture->home, editor_id);
	CHECK(access(parked_saved, F_OK) == -1 && errno == ENOENT);
	CHECK(access(foreground_saved, F_OK) == -1 && errno == ENOENT);
	uint64_t injected_time = fixture->now_ms;
	uint64_t started = real_ms();

	fixture->tearing_down = true;
	fplinux_session_destroy(fixture->session);
	fixture->session = NULL;
	uint64_t elapsed_ms = real_ms() - started;

	expect_file(parked_saved, "saved\n");
	expect_file(foreground_saved, "saved\n");
	CHECK(elapsed_ms >= 900 && elapsed_ms < 2500);
	CHECK(fixture->now_ms == injected_time);
	CHECK(!fixture->teardown_callbacks);
	for (size_t i = 0; i < fixture->peer_count; ++i) {
		errno = 0;
		CHECK(waitpid(fixture->peers[i].pid, NULL, WNOHANG) == -1 &&
		      errno == ECHILD);
	}
}

static struct fplinux_ui_session_message latest_tasks(struct fixture *fixture,
						      struct peer *launcher)
{
	struct fplinux_ui_session_message snapshot =
		wait_message(fixture, launcher, FPLINUX_UI_SESSION_TASKS, NULL);
	struct fplinux_ui_session_message next;

	while (take_message(launcher, FPLINUX_UI_SESSION_TASKS, NULL, &next))
		snapshot = next;
	CHECK(snapshot.has_tasks);
	return snapshot;
}

static struct fplinux_ui_session_message query_tasks(struct fixture *fixture,
						     struct peer *launcher)
{
	clear_messages(fixture, launcher);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_TASKS);
	return latest_tasks(fixture, launcher);
}

static struct fplinux_ui_session_message
wait_task_count(struct fixture *fixture, struct peer *launcher,
		unsigned int expected_count)
{
	struct fplinux_ui_session_message snapshot;
	uint64_t deadline = real_ms() + 2000;

	do {
		while (take_message(launcher, FPLINUX_UI_SESSION_TASKS, NULL,
				    &snapshot)) {
			CHECK(snapshot.has_tasks);
			if (snapshot.task_count == expected_count)
				return snapshot;
		}
		pump(fixture, 2);
	} while (real_ms() < deadline);
	CHECK(false);
	return (struct fplinux_ui_session_message){ 0 };
}

static const struct fplinux_ui_session_task *
task_record(const struct fplinux_ui_session_message *snapshot, const char *id)
{
	for (unsigned int i = 0; i < snapshot->task_count; ++i)
		if (!strcmp(snapshot->tasks[i].app_id, id))
			return &snapshot->tasks[i];
	CHECK(false);
	return NULL;
}

static void task_action(struct fixture *fixture, struct peer *launcher,
			enum fplinux_ui_session_message_type type,
			const char *id, uint32_t token)
{
	struct fplinux_ui_session_message message = {
		.type = type,
		.token = token,
	};

	strcpy(message.app_id, id);
	issue(fixture, launcher, &message);
}

static void return_home(struct fixture *fixture, struct peer *launcher)
{
	fplinux_session_home(fixture->session);
	struct fplinux_ui_session_message home =
		active_for(fixture, launcher, "");

	acknowledge(fixture, launcher, &home);
}

static void hold_activation(struct fixture *fixture, struct peer *peer)
{
	struct command command = {
		.type = COMMAND_HOLD_ACTIVATION,
		.message.enabled = true,
	};
	unsigned int sent = peer->sent;
	uint64_t deadline = real_ms() + 2000;

	send_command(peer->fd, &command);
	while (peer->sent == sent && real_ms() < deadline)
		pump(fixture, 2);
	CHECK(peer->sent > sent);
}

static void acknowledge_activation(struct fixture *fixture, struct peer *app,
				   uint32_t serial)
{
	struct fplinux_ui_session_message ack = {
		.type = FPLINUX_UI_SESSION_ACTIVATION_ACK,
		.serial = serial,
	};

	issue(fixture, app, &ack);
}

static void test_activation_intent_and_frame_barrier(struct fixture *fixture)
{
	const uint16_t catalogue[] = { 0x1111, 0x2222, 0x3333, 0x4444 };
	const uint16_t activated[] = { 0x07e0, 0xffff, 0xf800, 0x001f };
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message cold =
		wait_message(fixture, app, FPLINUX_UI_SESSION_ACTIVATION, NULL);
	uint32_t token =
		fplinux_session_find_process(fixture->session, app->pid)->token;
	uint32_t previous_serial = cold.serial;

	CHECK(cold.activation == FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	CHECK(cold.serial != 0);
	hold_activation(fixture, app);
	for (unsigned int i = 0; i < 4; ++i) {
		bool resume = i % 2;

		return_home(fixture, launcher);
		clear_messages(fixture, launcher);
		clear_messages(fixture, app);
		/* Process age cannot select navigation or exhaust a new operation. */
		fixture->now_ms += i >= 2 ? 3600000 : 1;
		/* A user wakes the display before choosing an app after inactivity. */
		fplinux_session_keyboard_key(fixture->session, KEY_F24, true);
		fplinux_session_keyboard_key(fixture->session, KEY_F24, false);
		present_scene(fixture, catalogue);
		if (resume)
			task_action(fixture, launcher,
				    FPLINUX_UI_SESSION_ACTIVATE, TEST_APP,
				    token);
		else
			CHECK(launch_app(fixture, launcher, TEST_APP) == app);
		struct fplinux_ui_session_message activation = wait_message(
			fixture, app, FPLINUX_UI_SESSION_ACTIVATION, NULL);
		struct fplinux_ui_session_message other_ack = {
			.type = FPLINUX_UI_SESSION_REDRAW_ACK,
			.serial = activation.serial,
		};
		int x, y, width, height;

		CHECK(activation.activation ==
		      (resume ? FPLINUX_UI_SESSION_ACTIVATION_RESUME :
				FPLINUX_UI_SESSION_ACTIVATION_OPEN));
		CHECK(activation.serial &&
		      activation.serial != previous_serial);
		previous_serial = activation.serial;
		CHECK(fplinux_session_find_process(fixture->session, app->pid)
			      ->token == token);
		fplinux_session_surface_box(fixture->session,
					    FPLINUX_UI_SESSION_INTEGRATED, &x,
					    &y, &width, &height);
		fplinux_session_surface_committed(fixture->session, app->pid,
						  width, height);
		present_scene(fixture, activated);
		issue(fixture, app, &other_ack);
		other_ack.type = FPLINUX_UI_SESSION_CLOSE_ACK;
		issue(fixture, app, &other_ack);
		acknowledge_activation(fixture, app, activation.serial + 1);
		/* The launcher's fallback may expire, but the app still owes ACK. */
		fixture->now_ms += 600;
		pump(fixture, 5);
		expect_scene(fixture, catalogue);
		CHECK(!fplinux_session_find_process(fixture->session, app->pid)
			       ->shown);
		CHECK(!fplinux_session_focused_pid(fixture->session));
		acknowledge_activation(fixture, app, activation.serial);
		struct fplinux_ui_session_message active =
			active_for(fixture, launcher, TEST_APP);

		acknowledge(fixture, launcher, &active);
		expect_scene(fixture, activated);
		CHECK(fplinux_session_focused_pid(fixture->session) ==
		      app->pid);
	}
}

static void test_resume_ended_task_during_handoff(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	struct fplinux_ui_session_message snapshot =
		query_tasks(fixture, launcher);
	uint32_t token = task_record(&snapshot, TEST_APP)->token;
	struct command crash = { .type = COMMAND_CRASH };

	task_action(fixture, launcher, FPLINUX_UI_SESSION_ACTIVATE, TEST_APP,
		    token);
	CHECK(fplinux_session_find_process(fixture->session, settings->pid)
		      ->shown);
	send_command(app->fd, &crash);
	wait_reaped(fixture, app->pid);
	struct fplinux_ui_session_message active;

	/* Reaping publishes a newer view, whose serial owns the leaving handoff. */
	do {
		active = active_for(fixture, launcher, "");
	} while (strcmp(active.reason, "app-crashed"));

	acknowledge(fixture, launcher, &active);
	struct fplinux_ui_session_message error =
		wait_message(fixture, launcher, FPLINUX_UI_SESSION_ERROR, NULL);

	CHECK(!strcmp(error.reason, "task-unavailable"));
	snapshot = query_tasks(fixture, launcher);
	CHECK(snapshot.task_count == 1);
	CHECK(!strcmp(snapshot.tasks[0].app_id, TEST_SETTINGS));
	CHECK(fixture->peer_count == 3);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
}

static void test_retained_activation_cancel(struct fixture *fixture,
					    const char *operation)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	uint32_t token =
		fplinux_session_find_process(fixture->session, app->pid)->token;

	return_home(fixture, launcher);
	clear_messages(fixture, app);
	hold_activation(fixture, app);
	CHECK(launch_app(fixture, launcher, TEST_APP) == app);
	struct fplinux_ui_session_message activation =
		wait_message(fixture, app, FPLINUX_UI_SESSION_ACTIVATION, NULL);

	if (!strcmp(operation, "home")) {
		send_type(fixture, launcher, FPLINUX_UI_SESSION_HOME);
	} else if (!strcmp(operation, "timeout")) {
		fixture->now_ms += 9999;
		pump(fixture, 5);
		CHECK(fplinux_session_find_process(fixture->session, app->pid)
			      ->lifecycle == FPLINUX_UI_SESSION_FOREGROUND);
		fixture->now_ms += 1;
		pump(fixture, 5);
	} else {
		send_type(fixture, launcher, FPLINUX_UI_SESSION_CANCEL);
	}
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	acknowledge(fixture, launcher, &active);
	acknowledge_activation(fixture, app, activation.serial);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, app->pid);
	struct fplinux_ui_session_message ignored;

	CHECK(process && process->token == token && !process->closing);
	CHECK(!process->shown &&
	      process->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	clear_messages(fixture, launcher);
	task_action(fixture, launcher, FPLINUX_UI_SESSION_ACTIVATE, TEST_APP,
		    token);
	struct fplinux_ui_session_message next =
		wait_message(fixture, app, FPLINUX_UI_SESSION_ACTIVATION, NULL);

	CHECK(next.serial != activation.serial);
	acknowledge_activation(fixture, app, activation.serial);
	CHECK(!fplinux_session_find_process(fixture->session, app->pid)->shown);
	acknowledge_activation(fixture, app, next.serial);
	active = active_for(fixture, launcher, TEST_APP);
	acknowledge(fixture, launcher, &active);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
}

static void
test_close_background_wait_activates_same_instance(struct fixture *fixture)
{
	const uint16_t initial[] = { 0x1111, 0x2222, 0x3333, 0x4444 };
	const uint16_t decision[] = { 0xffff, 0x0000, 0xf800, 0x07e0 };
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	struct fplinux_ui_session_message tasks =
		query_tasks(fixture, launcher);
	uint32_t token = task_record(&tasks, TEST_APP)->token;

	present_scene(fixture, initial);
	set_close_decision(fixture, app, "wait");
	task_action(fixture, launcher, FPLINUX_UI_SESSION_CLOSE_TASK, TEST_APP,
		    token);
	struct fplinux_ui_session_message prepare = wait_message(
		fixture, app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);
	struct fplinux_ui_session_message parked =
		active_for(fixture, launcher, "");

	acknowledge(fixture, launcher, &parked);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_APP);

	acknowledge(fixture, launcher, &active);
	/* Becoming visible is allowed before the decision's frame acknowledgment. */
	struct fplinux_ui_session_message visibility;

	do {
		visibility = wait_message(fixture, app,
					  FPLINUX_UI_SESSION_VISIBILITY, NULL);
	} while (!visibility.visible);
	present_scene(fixture, decision);
	expect_scene(fixture, initial);
	acknowledge_close(fixture, settings, prepare.serial);
	expect_scene(fixture, initial);
	acknowledge_close(fixture, app, prepare.serial + 1);
	expect_scene(fixture, initial);
	acknowledge_close(fixture, app, prepare.serial);
	expect_scene(fixture, decision);
	CHECK(active.token == token);
	CHECK(fixture->peer_count == 3);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	CHECK(fplinux_session_find_process(fixture->session, settings->pid)
		      ->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	answer_close(fixture, app, prepare.serial, "cancel");
	CHECK(!fplinux_session_find_process(fixture->session, app->pid)
		       ->closing);
}

static void test_task_controls(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct fplinux_ui_session_message snapshot =
		query_tasks(fixture, launcher);

	CHECK(!snapshot.task_count);
	struct peer *app = show_app(fixture, launcher, TEST_APP);

	snapshot = query_tasks(fixture, launcher);
	CHECK(snapshot.task_count == 1);
	const struct fplinux_ui_session_task *task =
		task_record(&snapshot, TEST_APP);
	uint32_t token = task->token;

	CHECK(token && !strcmp(task->title, TEST_APP));
	CHECK(task->lifecycle == FPLINUX_UI_SESSION_FOREGROUND);
	clear_messages(fixture, launcher);
	return_home(fixture, launcher);
	snapshot = latest_tasks(fixture, launcher);
	task = task_record(&snapshot, TEST_APP);
	CHECK(task->token == token &&
	      task->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	task_action(fixture, launcher, FPLINUX_UI_SESSION_ACTIVATE, TEST_APP,
		    token);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_APP);

	acknowledge(fixture, launcher, &active);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	CHECK(fixture->peer_count == 2);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	struct fplinux_ui_session_message title = {
		.type = FPLINUX_UI_SESSION_TITLE,
		.title = "Retained note",
	};

	clear_messages(fixture, launcher);
	issue(fixture, app, &title);
	snapshot = latest_tasks(fixture, launcher);
	CHECK(snapshot.task_count == 2);
	task = task_record(&snapshot, TEST_APP);
	CHECK(!strcmp(task->title, "Retained note") && task->token == token);
	CHECK(task->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	uint32_t settings_token = task_record(&snapshot, TEST_SETTINGS)->token;

	CHECK(settings_token && settings_token != token);
	clear_messages(fixture, launcher);
	task_action(fixture, launcher, FPLINUX_UI_SESSION_CLOSE_TASK, TEST_APP,
		    token);
	wait_message(fixture, app, FPLINUX_UI_SESSION_CLOSE, NULL);
	wait_reaped(fixture, app->pid);
	snapshot = wait_task_count(fixture, launcher, 1);
	CHECK(snapshot.task_count == 1);
	CHECK(!strcmp(snapshot.tasks[0].app_id, TEST_SETTINGS));
	CHECK(fplinux_session_focused_pid(fixture->session) == settings->pid);
	char saved[PATH_MAX];

	path_join(saved, fixture->home, TEST_APP);
	expect_file(saved, "saved\n");
	/* Closing during the Home handoff must override pending retention. */
	fplinux_session_home(fixture->session);
	active_for(fixture, launcher, "");
	task_action(fixture, launcher, FPLINUX_UI_SESSION_CLOSE_TASK,
		    TEST_SETTINGS, settings_token);
	wait_message(fixture, settings, FPLINUX_UI_SESSION_CLOSE, NULL);
	wait_reaped(fixture, settings->pid);
	snapshot = query_tasks(fixture, launcher);
	CHECK(!snapshot.task_count);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
}

static void test_task_localized_title(struct fixture *fixture)
{
	char path[PATH_MAX];
	FILE *file;

	path_join(path, fixture->applications, TEST_APP ".desktop");
	file = fopen(path, "a");
	CHECK(file);
	CHECK(fputs("Name=Notes\nName[ru]=Заметки\n", file) >= 0);
	CHECK(fclose(file) == 0);
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message snapshot =
		query_tasks(fixture, launcher);

	CHECK(!strcmp(task_record(&snapshot, TEST_APP)->title, "Заметки"));
	struct fplinux_ui_session_message title = {
		.type = FPLINUX_UI_SESSION_TITLE,
		.title = "Черновик",
	};

	issue(fixture, app, &title);
	snapshot = query_tasks(fixture, launcher);
	CHECK(!strcmp(task_record(&snapshot, TEST_APP)->title, "Черновик"));
	return_home(fixture, launcher);
	snapshot = query_tasks(fixture, launcher);
	CHECK(!strcmp(task_record(&snapshot, TEST_APP)->title, "Черновик"));
}

static void test_task_stale_and_locked(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message snapshot =
		query_tasks(fixture, launcher);
	uint32_t old_token = task_record(&snapshot, TEST_APP)->token;
	struct command crash = { .type = COMMAND_CRASH };
	pid_t old_pid = app->pid;

	return_home(fixture, launcher);
	clear_messages(fixture, launcher);
	send_command(app->fd, &crash);
	wait_reaped(fixture, old_pid);
	snapshot = wait_task_count(fixture, launcher, 0);
	CHECK(!snapshot.task_count);
	app = show_app(fixture, launcher, TEST_APP);
	CHECK(app->pid != old_pid);
	snapshot = query_tasks(fixture, launcher);
	uint32_t new_token = task_record(&snapshot, TEST_APP)->token;

	CHECK(new_token && new_token != old_token);
	return_home(fixture, launcher);
	const enum fplinux_ui_session_message_type actions[] = {
		FPLINUX_UI_SESSION_CLOSE_TASK,
		FPLINUX_UI_SESSION_ACTIVATE,
	};

	for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
		clear_messages(fixture, launcher);
		task_action(fixture, launcher, actions[i], TEST_APP, old_token);
		struct fplinux_ui_session_message error = wait_message(
			fixture, launcher, FPLINUX_UI_SESSION_ERROR, NULL);

		CHECK(!strcmp(error.reason, "task-unavailable"));
		snapshot = latest_tasks(fixture, launcher);
		CHECK(task_record(&snapshot, TEST_APP)->token == new_token);
		const struct fplinux_session_process *process =
			fplinux_session_find_process(fixture->session,
						     app->pid);

		CHECK(process && !process->closing &&
		      process->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
		CHECK(fplinux_session_focused_pid(fixture->session) ==
		      launcher->pid);
	}
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	struct fplinux_ui_session_message setting = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.name = "auto_lock_seconds",
		.value = "0",
	};

	issue(fixture, settings, &setting);
	wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
		     "auto_lock_seconds");
	return_home(fixture, launcher);
	fixture->now_ms += 30000;
	pump(fixture, 4);
	CHECK(fplinux_session_is_locked(fixture->session));
	for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
		clear_messages(fixture, launcher);
		task_action(fixture, launcher, actions[i], TEST_APP, new_token);
		struct fplinux_ui_session_message error = wait_message(
			fixture, launcher, FPLINUX_UI_SESSION_ERROR, NULL);

		CHECK(!strcmp(error.reason, "task-unavailable"));
		const struct fplinux_session_process *process =
			fplinux_session_find_process(fixture->session,
						     app->pid);

		CHECK(process && !process->closing &&
		      process->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	}
	/* A regular authenticated application cannot query the task registry. */
	struct command query = {
		.type = COMMAND_SEND,
		.message.type = FPLINUX_UI_SESSION_TASKS,
	};

	send_command(app->fd, &query);
	wait_reaped(fixture, app->pid);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_TASKS, NULL, &snapshot));
	CHECK(fplinux_session_find_process(fixture->session, launcher->pid));
	CHECK(fplinux_session_find_process(fixture->session, settings->pid));
}

static void test_first_buffer_handoff(struct fixture *fixture, bool ready_first)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = launch_app(fixture, launcher, TEST_APP);
	CHECK(fixture->frame_held);
	struct fplinux_ui_session_message config =
		authenticate(fixture, app, NULL);
	int x, y, width, height;

	fplinux_session_surface_box(fixture->session, config.mode, &x, &y,
				    &width, &height);

	if (ready_first) {
		send_type(fixture, app, FPLINUX_UI_SESSION_READY);
		fplinux_session_frame(fixture->session, app->pid);
		CHECK(!fplinux_session_find_process(fixture->session, app->pid)
			       ->shown);
	}
	fplinux_session_surface_committed(fixture->session, app->pid, width,
					  height);
	if (!ready_first) {
		struct fplinux_ui_session_message message;

		fplinux_session_frame(fixture->session, app->pid);
		pump(fixture, 10);
		CHECK(!fplinux_session_find_process(fixture->session, app->pid)
			       ->shown);
		while (take_message(launcher, FPLINUX_UI_SESSION_ACTIVE, NULL,
				    &message))
			CHECK(!message.active_id[0]);
		send_type(fixture, app, FPLINUX_UI_SESSION_READY);
	}
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_APP);

	CHECK(!fplinux_session_find_process(fixture->session, app->pid)->shown);
	struct fplinux_ui_session_message ack = {
		.type = FPLINUX_UI_SESSION_ACTIVE_ACK,
		.serial = active.serial - 1,
	};

	issue(fixture, launcher, &ack);
	CHECK(!fplinux_session_find_process(fixture->session, app->pid)->shown);
	acknowledge(fixture, launcher, &active);
	CHECK(!fixture->frame_held);
	CHECK(fplinux_session_find_process(fixture->session, app->pid)->shown);
	clear_messages(fixture, launcher);
	fplinux_session_home(fixture->session);
	active_for(fixture, launcher, "");
	CHECK(fixture->frame_held);
	CHECK(fplinux_session_find_process(fixture->session, app->pid)->shown);
	fixture->now_ms += 499;
	pump(fixture, 4);
	CHECK(fplinux_session_find_process(fixture->session, app->pid)->shown);
	fixture->now_ms += 1;
	pump(fixture, 4);
	CHECK(!fplinux_session_find_process(fixture->session, app->pid)->shown);
}

static void test_peer_authentication(struct fixture *fixture)
{
	struct peer *launcher = wait_peer(fixture, TEST_LAUNCHER);
	struct command wrong = { .type = COMMAND_CONNECT };
	uint64_t deadline = real_ms() + 2000;

	strcpy(wrong.app_id, TEST_APP);
	send_command(launcher->fd, &wrong);
	while (!launcher->disconnected && real_ms() < deadline)
		pump(fixture, 2);
	CHECK(launcher->disconnected);
	struct fplinux_ui_session_message message;

	CHECK(!take_message(launcher, FPLINUX_UI_SESSION_CONFIG, NULL,
			    &message));
	authenticate(fixture, launcher, NULL);
	int impostor = fplinux_ui_session_connect_at(fixture->socket_path,
						     TEST_LAUNCHER);

	CHECK(impostor >= 0);
	pump(fixture, 20);
	CHECK(fplinux_ui_session_receive(impostor, &message) == -1);
	close(impostor);
	CHECK(!fplinux_session_find_process(fixture->session, getpid()));
}

static void test_cancel_cleans_process(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = launch_app(fixture, launcher, TEST_APP);
	struct fplinux_session_launch progress;
	struct command hold = { .type = COMMAND_HOLD_CLOSE };
	unsigned int sent;
	uint64_t deadline;

	authenticate(fixture, app, NULL);
	sent = app->sent;
	deadline = real_ms() + 2000;
	send_command(app->fd, &hold);
	while (app->sent == sent && real_ms() < deadline)
		pump(fixture, 2);
	CHECK(app->sent > sent);
	fplinux_session_get_launch(fixture->session, &progress);
	CHECK(progress.active && !progress.shown && progress.generation != 0);
	fixture->now_ms += 199;
	pump(fixture, 4);
	fplinux_session_get_launch(fixture->session, &progress);
	CHECK(progress.active && !progress.shown);
	fixture->now_ms += 1;
	pump(fixture, 4);
	fplinux_session_get_launch(fixture->session, &progress);
	CHECK(progress.active && progress.shown);
	clear_messages(fixture, launcher);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_CANCEL);
	wait_message(fixture, app, FPLINUX_UI_SESSION_CLOSE, NULL);
	fplinux_session_get_launch(fixture->session, &progress);
	CHECK(!progress.active && !progress.shown);
	fixture->now_ms += 500;
	pump(fixture, 4);
	CHECK(fplinux_session_find_process(fixture->session, app->pid));
	fixture->now_ms += 500;
	pump(fixture, 4);
	wait_reaped(fixture, app->pid);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	CHECK(!strcmp(active.value, "catalogue"));
	CHECK(!strcmp(active.reason, ""));
}

static bool phone_gesture(struct fixture *fixture, unsigned int code)
{
	bool press = fplinux_session_key(fixture->session, code, true,
					 fixture->now_ms);
	bool release = fplinux_session_key(fixture->session, code, false,
					   fixture->now_ms + 1);
	pump(fixture, 10);

	return press && release;
}

static void test_red_home_retains_task(struct fixture *fixture, bool fullscreen)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app;
	struct fplinux_ui_session_message ignored;
	pid_t pid;

	if (fullscreen) {
		add_entry(fixture, TEST_APP, "fullscreen");
		app = launch_app(fixture, launcher, TEST_APP);
		struct fplinux_ui_session_message config =
			authenticate(fixture, app, NULL);

		commit_app(fixture, app, &config);
		wait_reaped(fixture, launcher->pid);
	} else {
		app = show_app(fixture, launcher, TEST_APP);
	}
	pid = app->pid;
	clear_messages(fixture, app);
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_POWER));
	if (fullscreen) {
		launcher = wait_peer(fixture, TEST_LAUNCHER);
		authenticate(fixture, launcher, NULL);
		send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
		fplinux_session_surface_committed(fixture->session,
						  launcher->pid, fixture->width,
						  fixture->height);
		fplinux_session_frame(fixture->session, launcher->pid);
	}
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	CHECK(!strcmp(active.value, "home"));
	acknowledge(fixture, launcher, &active);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, pid);

	CHECK(process && !process->closing && !process->shown);
	CHECK(process->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL,
			    &ignored));
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
}

static void select_launcher_view(struct fixture *fixture, struct peer *launcher,
				 const char *view, const char *presentation)
{
	struct fplinux_ui_session_message select = {
		.type = FPLINUX_UI_SESSION_SELECT,
		.selected_id = TEST_APP,
	};

	strcpy(select.value, view);
	strcpy(select.name, presentation);
	issue(fixture, launcher, &select);
}

static void set_tasks_view(struct fixture *fixture, struct peer *launcher,
			   bool opened)
{
	struct fplinux_ui_session_message select = {
		.type = FPLINUX_UI_SESSION_SELECT,
		.value = "tasks",
		.enabled = opened,
		.selected_id = TEST_APP,
		.name = "transient-only",
	};

	issue(fixture, launcher, &select);
}

static void test_task_view_red_returns_home(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct fplinux_ui_session_message title = {
		.type = FPLINUX_UI_SESSION_TITLE,
		.title = "Task snapshot",
	};

	select_launcher_view(fixture, launcher, "home", "grid");
	set_tasks_view(fixture, launcher, true);
	/* A catalogue refresh reports its selection while Tasks stays open. */
	select_launcher_view(fixture, launcher, "home", "grid");
	struct fplinux_ui_session_message snapshot =
		query_tasks(fixture, launcher);

	CHECK(snapshot.task_count == 0);
	issue(fixture, launcher, &title);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	CHECK(!strcmp(active.value, "home"));
	CHECK(!strcmp(active.name, "grid"));
	CHECK(!strcmp(active.selected_id, TEST_APP));
	acknowledge(fixture, launcher, &active);
	clear_messages(fixture, launcher);
	phone_gesture(fixture, FPLINUX_KEY_POWER);
	wait_message(fixture, launcher, FPLINUX_UI_SESSION_HOME, NULL);
	CHECK(!fplinux_session_is_locked(fixture->session));
	expect_number(fixture->brightness_effective, 7);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);

	clear_messages(fixture, launcher);
	phone_gesture(fixture, FPLINUX_KEY_POWER);
	CHECK(fplinux_session_is_locked(fixture->session));
	expect_number(fixture->brightness_effective, 0);
	active = active_for(fixture, launcher, "");
	CHECK(active.locked &&
	      active.display == FPLINUX_UI_SESSION_DISPLAY_OFF);
	CHECK(!take_message(launcher, FPLINUX_UI_SESSION_HOME, NULL, &active));
}

static void test_task_view_closed_home_locks(struct fixture *fixture,
					     bool opened)
{
	struct peer *launcher = start_launcher(fixture);

	select_launcher_view(fixture, launcher, "home", "grid");
	if (opened)
		set_tasks_view(fixture, launcher, true);
	query_tasks(fixture, launcher);
	if (opened)
		set_tasks_view(fixture, launcher, false);
	clear_messages(fixture, launcher);
	phone_gesture(fixture, FPLINUX_KEY_POWER);
	CHECK(fplinux_session_is_locked(fixture->session));
	expect_number(fixture->brightness_effective, 0);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	CHECK(active.locked &&
	      active.display == FPLINUX_UI_SESSION_DISPLAY_OFF);
	CHECK(!take_message(launcher, FPLINUX_UI_SESSION_HOME, NULL, &active));
}

static void test_task_view_restart_restores_base(struct fixture *fixture,
						 bool catalogue)
{
	const char *view = catalogue ? "catalogue" : "home";
	const char *presentation = catalogue ? "list" : "grid";
	struct peer *launcher = start_launcher(fixture);
	struct command crash = { .type = COMMAND_CRASH };
	struct fplinux_ui_session_message title = {
		.type = FPLINUX_UI_SESSION_TITLE,
		.title = "Task snapshot",
	};

	select_launcher_view(fixture, launcher, view, presentation);
	set_tasks_view(fixture, launcher, true);
	clear_messages(fixture, launcher);
	issue(fixture, launcher, &title);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	CHECK(!strcmp(active.value, view));
	CHECK(!strcmp(active.name, presentation));
	CHECK(!strcmp(active.selected_id, TEST_APP));
	acknowledge(fixture, launcher, &active);
	send_command(launcher->fd, &crash);
	wait_reaped(fixture, launcher->pid);
	launcher = wait_peer(fixture, TEST_LAUNCHER);
	authenticate(fixture, launcher, NULL);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	fplinux_session_frame(fixture->session, launcher->pid);
	active = active_for(fixture, launcher, "");
	CHECK(!strcmp(active.value, view));
	CHECK(!strcmp(active.name, presentation));
	CHECK(!strcmp(active.selected_id, TEST_APP));
	acknowledge(fixture, launcher, &active);
	clear_messages(fixture, launcher);
	phone_gesture(fixture, FPLINUX_KEY_POWER);
	if (catalogue) {
		wait_message(fixture, launcher, FPLINUX_UI_SESSION_HOME, NULL);
		CHECK(!fplinux_session_is_locked(fixture->session));
		expect_number(fixture->brightness_effective, 7);
		CHECK(fplinux_session_focused_pid(fixture->session) ==
		      launcher->pid);
	} else {
		CHECK(fplinux_session_is_locked(fixture->session));
		expect_number(fixture->brightness_effective, 0);
		CHECK(!take_message(launcher, FPLINUX_UI_SESSION_HOME, NULL,
				    &active));
	}
}

static void
test_task_view_close_preserves_activated_selection(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_SETTINGS);

	return_home(fixture, launcher);
	struct fplinux_ui_session_message snapshot =
		query_tasks(fixture, launcher);
	uint32_t token = task_record(&snapshot, TEST_SETTINGS)->token;

	select_launcher_view(fixture, launcher, "catalogue", "list");
	set_tasks_view(fixture, launcher, true);
	task_action(fixture, launcher, FPLINUX_UI_SESSION_ACTIVATE,
		    TEST_SETTINGS, token);
	set_tasks_view(fixture, launcher, false);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_SETTINGS);

	CHECK(!strcmp(active.selected_id, TEST_SETTINGS));
	acknowledge(fixture, launcher, &active);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	clear_messages(fixture, launcher);
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	active = active_for(fixture, launcher, "");
	CHECK(!strcmp(active.value, "catalogue"));
	CHECK(!strcmp(active.name, "list"));
	CHECK(!strcmp(active.selected_id, TEST_SETTINGS));
	acknowledge(fixture, launcher, &active);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
}

static void setting(struct fixture *fixture, struct peer *settings,
		    const char *name, const char *value)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SETTING
	};

	strcpy(message.name, name);
	strcpy(message.value, value);
	issue(fixture, settings, &message);
}

static void test_task_titles_follow_applied_locale(struct fixture *fixture,
						   bool explicit_title,
						   bool save_failed)
{
	char path[PATH_MAX];
	char source[PATH_MAX];
	FILE *file;

	path_join(source, fixture->root, "system-language-en");
	path_join(path, fixture->languages, "en");
	CHECK(rename(source, path) == 0);
	path_join(path, fixture->applications, TEST_SETTINGS ".desktop");
	file = fopen(path, "a");
	CHECK(file);
	CHECK(fputs("Name=Settings\nName[ru]=Настройки\n", file) >= 0);
	CHECK(fclose(file) == 0);
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	struct fplinux_ui_session_message snapshot =
		query_tasks(fixture, launcher);
	uint32_t token = task_record(&snapshot, TEST_SETTINGS)->token;

	CHECK(!strcmp(task_record(&snapshot, TEST_SETTINGS)->title,
		      "Настройки"));
	if (explicit_title) {
		struct fplinux_ui_session_message title = {
			.type = FPLINUX_UI_SESSION_TITLE,
			.title = "Настройки",
		};

		issue(fixture, settings, &title);
	}
	/* Queries, rejected values and keypad language do not rename the task. */
	setting(fixture, settings, "locale", "");
	struct fplinux_ui_session_message reply = wait_message(
		fixture, settings, FPLINUX_UI_SESSION_SETTING, "locale");

	CHECK(!reply.serial && !strcmp(reply.value, "ru"));
	setting(fixture, settings, "locale", "xx");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_ERROR,
			     "locale");
	CHECK(!reply.applied && !reply.serial);
	setting(fixture, settings, "input_locale", "ru");
	wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
		     "input_locale");
	snapshot = query_tasks(fixture, launcher);
	CHECK(!strcmp(task_record(&snapshot, TEST_SETTINGS)->title,
		      "Настройки"));
	for (unsigned int i = 0; i < 2; ++i) {
		const char *locale = i ? "ru" : "en";
		const char *title = explicit_title || i ? "Настройки" :
							  "Settings";
		pid_t old_launcher = launcher->pid;

		clear_messages(fixture, settings);
		sync_fails = save_failed;
		setting(fixture, settings, "locale", locale);
		reply = wait_message(fixture, settings,
				     save_failed ? FPLINUX_UI_SESSION_ERROR :
						   FPLINUX_UI_SESSION_SETTING,
				     "locale");
		sync_fails = false;
		CHECK(reply.applied && reply.serial);
		CHECK(!strcmp(reply.value, locale));
		if (save_failed)
			CHECK(!strcmp(reply.reason, "save-failed"));
		wait_reaped(fixture, old_launcher);
		launcher = wait_peer(fixture, TEST_LAUNCHER);
		struct fplinux_ui_session_message config =
			authenticate(fixture, launcher, NULL);

		CHECK(!strcmp(config.locale, locale));
		send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
		fplinux_session_surface_committed(fixture->session,
						  launcher->pid, fixture->width,
						  fixture->height);
		fplinux_session_frame(fixture->session, launcher->pid);
		struct fplinux_ui_session_message active =
			active_for(fixture, launcher, TEST_SETTINGS);
		struct fplinux_ui_session_message redraw = {
			.type = FPLINUX_UI_SESSION_REDRAW_ACK,
			.serial = reply.serial,
		};

		acknowledge(fixture, launcher, &active);
		issue(fixture, settings, &redraw);
		CHECK(!strcmp(active.title, title) && active.token == token);
		snapshot = query_tasks(fixture, launcher);
		CHECK(!strcmp(task_record(&snapshot, TEST_SETTINGS)->title,
			      title));
		CHECK(task_record(&snapshot, TEST_SETTINGS)->token == token);
		CHECK(fplinux_session_focused_pid(fixture->session) ==
		      settings->pid);
		struct fplinux_ui_session_message ignored;

		CHECK(!take_message(settings, FPLINUX_UI_SESSION_ACTIVATION,
				    NULL, &ignored));
	}
}

static void test_key_routing(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);

	clear_messages(fixture, app);
	CHECK(phone_gesture(fixture, FPLINUX_KEY_CALL));
	clear_messages(fixture, app);
	fixture->now_ms += 30000;
	pump(fixture, 4);
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_2));
	CHECK(fplinux_session_keyboard_key(fixture->session, KEY_A, true));
	CHECK(fplinux_session_keyboard_key(fixture->session, KEY_A, false));
	CHECK(phone_gesture(fixture, FPLINUX_KEY_2));
	fixture->now_ms += 30000;
	pump(fixture, 4);
	CHECK(!fplinux_session_key(fixture->session, FPLINUX_KEY_2, true,
				   fixture->now_ms));
	fplinux_session_cancel_input(fixture->session,
				     FPLINUX_SESSION_KEY_PHONE);
	CHECK(phone_gesture(fixture, FPLINUX_KEY_2));
	fixture->now_ms += 30000;
	pump(fixture, 4);
	CHECK(!fplinux_session_keyboard_key(fixture->session, KEY_A, true));
	fplinux_session_cancel_input(fixture->session,
				     FPLINUX_SESSION_KEY_KEYBOARD);
	CHECK(fplinux_session_keyboard_key(fixture->session, KEY_A, true));
	CHECK(fplinux_session_keyboard_key(fixture->session, KEY_A, false));
	CHECK(!fplinux_session_key(fixture->session, FPLINUX_KEY_POWER, true,
				   fixture->now_ms));
	fplinux_session_cancel_input(fixture->session,
				     FPLINUX_SESSION_KEY_PHONE);
	CHECK(!fplinux_session_key(fixture->session, FPLINUX_KEY_POWER, false,
				   fixture->now_ms + 1));
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);

	setting(fixture, settings, "auto_lock_seconds", "0");
	wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
		     "auto_lock_seconds");
	clear_messages(fixture, launcher);
	clear_messages(fixture, settings);
	fixture->now_ms += 30000;
	pump(fixture, 4);
	CHECK(fplinux_session_is_locked(fixture->session));
	CHECK(!fplinux_session_keyboard_key(fixture->session, KEY_F13, true));
	CHECK(!fplinux_session_keyboard_key(fixture->session, KEY_F13, false));
	CHECK(!fplinux_session_keyboard_key(fixture->session, KEY_NUMERIC_STAR,
					    true));
	CHECK(!fplinux_session_keyboard_key(fixture->session, KEY_NUMERIC_STAR,
					    false));
	CHECK(fplinux_session_is_locked(fixture->session));
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_SOFT_LEFT));
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_STAR));
	CHECK(!fplinux_session_is_locked(fixture->session));
	CHECK(fplinux_session_keyboard_key(fixture->session, KEY_A, true));
	CHECK(fplinux_session_keyboard_key(fixture->session, KEY_A, false));
}

static void test_input_language(struct fixture *fixture, bool save_fails)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	pid_t app_pid = app->pid;
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	pid_t launcher_pid = launcher->pid;

	clear_messages(fixture, app);
	clear_messages(fixture, launcher);
	sync_fails = save_fails;
	setting(fixture, settings, "input_locale", "ru");
	struct fplinux_ui_session_message reply =
		wait_message(fixture, settings,
			     save_fails ? FPLINUX_UI_SESSION_ERROR :
					  FPLINUX_UI_SESSION_SETTING,
			     "input_locale");

	CHECK(!strcmp(reply.value, "ru"));
	CHECK(!strcmp(reply.reason, save_fails ? "save-failed" : ""));
	reply = wait_message(fixture, app, FPLINUX_UI_SESSION_SETTING,
			     "input_locale");
	CHECK(!strcmp(reply.value, "ru"));
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_SETTINGS);

	CHECK(!strcmp(active.locale, "en") &&
	      !strcmp(active.input_locale, "ru"));
	acknowledge(fixture, launcher, &active);
	CHECK(fplinux_session_find_process(fixture->session, app_pid));
	CHECK(fplinux_session_find_process(fixture->session, launcher_pid));
	setting(fixture, settings, "locale", "");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
			     "locale");
	CHECK(!strcmp(reply.value, "en"));
	setting(fixture, settings, "input_locale", "");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
			     "input_locale");
	CHECK(!strcmp(reply.value, "ru"));
	if (save_fails)
		expect_file(
			fixture->settings,
			"screen_off_seconds=30\nauto_lock_seconds=off\n"
			"locale=en\ninput_locale=en\ntheme=light\nbrightness=7\n");
	else
		expect_file(
			fixture->settings,
			"screen_off_seconds=30\nauto_lock_seconds=off\n"
			"locale=en\ninput_locale=ru\ntheme=light\nbrightness=7\n");
	add_entry(fixture, "org.fplinux.reader", "integrated");
	struct peer *reader =
		launch_app(fixture, launcher, "org.fplinux.reader");
	struct fplinux_ui_session_message config =
		authenticate(fixture, reader, NULL);

	CHECK(!strcmp(config.locale, "en") &&
	      !strcmp(config.input_locale, "ru"));
}

static void test_settings_retries_latest_value(struct fixture *fixture,
					       bool explicit)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);

	sync_fails = true;
	setting(fixture, settings, "auto_lock_seconds", "5");
	struct fplinux_ui_session_message reply =
		wait_message(fixture, settings, FPLINUX_UI_SESSION_ERROR,
			     "auto_lock_seconds");

	CHECK(reply.applied && !strcmp(reply.value, "5"));
	CHECK(reply.storage_state == FPLINUX_STORAGE_UNPUBLISHED &&
	      reply.storage_error == EIO);
	setting(fixture, settings, "auto_lock_seconds", "30");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_ERROR,
			     "auto_lock_seconds");
	CHECK(reply.applied && !strcmp(reply.value, "30"));
	fixture->now_ms += 2000;
	pump(fixture, 4);
	sync_fails = false;
	if (explicit) {
		setting(fixture, settings, "auto_lock_seconds", "30");
		reply = wait_message(fixture, settings,
				     FPLINUX_UI_SESSION_SETTING,
				     "auto_lock_seconds");
	} else {
		fixture->now_ms += 4000;
		pump(fixture, 4);
		setting(fixture, settings, "auto_lock_seconds", "");
		reply = wait_message(fixture, settings,
				     FPLINUX_UI_SESSION_SETTING,
				     "auto_lock_seconds");
	}
	CHECK(reply.applied && !strcmp(reply.value, "30"));
	CHECK(reply.storage_state == FPLINUX_STORAGE_DURABLE &&
	      !reply.storage_error);
	expect_file(fixture->settings,
		    "screen_off_seconds=30\nauto_lock_seconds=30\n"
		    "locale=en\ninput_locale=en\ntheme=light\nbrightness=7\n");
}

static void test_brightness_restore(struct fixture *fixture, bool crash)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);

	setting(fixture, settings, "brightness_preview", "3");
	struct fplinux_ui_session_message reply =
		wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
			     "brightness_preview");

	CHECK(!strcmp(reply.value, "3"));
	expect_file(fixture->brightness_level, "3\n");
	expect_file(fixture->settings,
		    "screen_off_seconds=30\nauto_lock_seconds=off\n"
		    "locale=en\ninput_locale=en\ntheme=light\nbrightness=7\n");
	if (crash) {
		struct command command = { .type = COMMAND_CRASH };

		send_command(settings->fd, &command);
		wait_reaped(fixture, settings->pid);
	} else {
		fplinux_session_home(fixture->session);
		pump(fixture, 10);
		CHECK(fplinux_session_find_process(fixture->session,
						   settings->pid));
	}
	expect_file(fixture->brightness_level, "7\n");
}

static void test_wake_stays_dark_before_presentation(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);

	setting(fixture, settings, "auto_lock_seconds", "0");
	wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
		     "auto_lock_seconds");
	fplinux_session_home(fixture->session);
	struct fplinux_ui_session_message home =
		active_for(fixture, launcher, "");

	acknowledge(fixture, launcher, &home);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
	fixture->now_ms += 30000;
	pump(fixture, 4);
	CHECK(fplinux_session_is_locked(fixture->session));
	expect_number(fixture->brightness_effective, 0);
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_2));
	expect_number(fixture->brightness_effective, 0);
	fixture->now_ms += 101;
	pump(fixture, 4);
	/* A timer cannot establish that the lock screen reached the display. */
	expect_number(fixture->brightness_effective, 0);
	struct fplinux_ui_session_message active;

	do {
		active = wait_message(fixture, launcher,
				      FPLINUX_UI_SESSION_ACTIVE, NULL);
	} while (!active.locked ||
		 active.display != FPLINUX_UI_SESSION_DISPLAY_ON);
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	expect_number(fixture->brightness_effective, 0);
	pid_t pids[] = { launcher->pid };

	/* A commit without the current ACTIVE_ACK cannot confirm the scene. */
	fplinux_session_output_latched(fixture->session, 10, pids, 1);
	acknowledge(fixture, launcher, &active);
	fplinux_session_output_presented(fixture->session, 10);
	expect_number(fixture->brightness_effective, 0);
	fplinux_session_output_latched(fixture->session, 11, pids, 1);
	expect_number(fixture->brightness_effective, 0);
	fplinux_session_output_presented(fixture->session, 0);
	fplinux_session_output_presented(fixture->session, 10);
	expect_number(fixture->brightness_effective, 0);
	fplinux_session_output_presented(fixture->session, 11);
	expect_number(fixture->brightness_effective, 7);
	CHECK(fplinux_session_is_locked(fixture->session));
}

static struct fplinux_ui_session_message
wake_active(struct fixture *fixture, struct peer *launcher, const char *id)
{
	struct fplinux_ui_session_message active;

	do {
		active = active_for(fixture, launcher, id);
	} while (active.display != FPLINUX_UI_SESSION_DISPLAY_ON);
	return active;
}

static void sleep_and_wake(struct fixture *fixture)
{
	fixture->now_ms += 30000;
	pump(fixture, 4);
	expect_number(fixture->brightness_effective, 0);
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_2));
	expect_number(fixture->brightness_effective, 0);
}

static void commit_launcher(struct fixture *fixture, struct peer *launcher)
{
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
}

static void test_wake_late_commit(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	pid_t pids[] = { launcher->pid };

	sleep_and_wake(fixture);
	struct fplinux_ui_session_message active =
		wake_active(fixture, launcher, "");

	/* The submitted frame still contains the buffer from before OFF. */
	fplinux_session_output_latched(fixture->session, 20, pids, 1);
	commit_launcher(fixture, launcher);
	acknowledge(fixture, launcher, &active);
	fplinux_session_output_presented(fixture->session, 20);
	expect_number(fixture->brightness_effective, 0);
	fplinux_session_output_latched(fixture->session, 21, pids, 1);
	expect_number(fixture->brightness_effective, 0);
	fplinux_session_output_presented(fixture->session, 21);
	expect_number(fixture->brightness_effective, 7);
}

static void test_wake_application_buffers(struct fixture *fixture,
					  bool fullscreen)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app;
	int x, y, width, height;

	if (fullscreen) {
		add_entry(fixture, TEST_APP, "fullscreen");
		app = launch_app(fixture, launcher, TEST_APP);
		struct fplinux_ui_session_message config =
			authenticate(fixture, app, NULL);

		CHECK(config.mode == FPLINUX_UI_SESSION_FULLSCREEN);
		commit_app(fixture, app, &config);
		wait_reaped(fixture, launcher->pid);
	} else {
		app = show_app(fixture, launcher, TEST_APP);
	}
	sleep_and_wake(fixture);
	CHECK(!fplinux_session_is_locked(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	fplinux_session_surface_box(fixture->session,
				    fullscreen ? FPLINUX_UI_SESSION_FULLSCREEN :
						 FPLINUX_UI_SESSION_INTEGRATED,
				    &x, &y, &width, &height);
	fplinux_session_surface_committed(fixture->session, app->pid, width,
					  height);
	pid_t pids[] = { app->pid, launcher->pid };

	if (!fullscreen) {
		/* An integrated application's bars need their own fresh buffer. */
		fplinux_session_output_latched(fixture->session, 30, pids, 2);
		fplinux_session_output_presented(fixture->session, 30);
		expect_number(fixture->brightness_effective, 0);
		struct fplinux_ui_session_message active =
			wake_active(fixture, launcher, TEST_APP);

		commit_launcher(fixture, launcher);
		acknowledge(fixture, launcher, &active);
		/* Fresh bars outside this output submission still do not count. */
		fplinux_session_output_latched(fixture->session, 31, pids, 1);
		fplinux_session_output_presented(fixture->session, 31);
		expect_number(fixture->brightness_effective, 0);
	}
	fplinux_session_output_latched(fixture->session, 32, pids,
				       fullscreen ? 1 : 2);
	fixture->now_ms += 101;
	pump(fixture, 4);
	expect_number(fixture->brightness_effective, 0);
	fplinux_session_output_presented(fixture->session, 31);
	expect_number(fixture->brightness_effective, 0);
	fplinux_session_output_presented(fixture->session, 32);
	expect_number(fixture->brightness_effective, 7);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
}

static void test_wake_output_reset(struct fixture *fixture, bool new_wake)
{
	struct peer *launcher = start_launcher(fixture);
	pid_t pids[] = { launcher->pid };

	sleep_and_wake(fixture);
	struct fplinux_ui_session_message active =
		wake_active(fixture, launcher, "");

	commit_launcher(fixture, launcher);
	acknowledge(fixture, launcher, &active);
	fplinux_session_output_latched(fixture->session, 40, pids, 1);
	if (new_wake)
		sleep_and_wake(fixture);
	else
		fplinux_session_output_reset(fixture->session);
	fplinux_session_output_presented(fixture->session, 40);
	expect_number(fixture->brightness_effective, 0);
	if (new_wake) {
		active = wake_active(fixture, launcher, "");
		commit_launcher(fixture, launcher);
		acknowledge(fixture, launcher, &active);
	}
	fplinux_session_output_latched(fixture->session, 41, pids, 1);
	fplinux_session_output_presented(fixture->session, 41);
	expect_number(fixture->brightness_effective, 7);
}

static void test_wake_launcher_restart(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct command crash = { .type = COMMAND_CRASH };
	pid_t pids[] = { launcher->pid };

	sleep_and_wake(fixture);
	struct fplinux_ui_session_message active =
		wake_active(fixture, launcher, "");

	commit_launcher(fixture, launcher);
	acknowledge(fixture, launcher, &active);
	fplinux_session_output_latched(fixture->session, 50, pids, 1);
	send_command(launcher->fd, &crash);
	wait_reaped(fixture, launcher->pid);
	launcher = wait_peer(fixture, TEST_LAUNCHER);
	authenticate(fixture, launcher, NULL);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	commit_launcher(fixture, launcher);
	fplinux_session_frame(fixture->session, launcher->pid);
	active = wake_active(fixture, launcher, "");
	acknowledge(fixture, launcher, &active);
	fplinux_session_output_presented(fixture->session, 50);
	expect_number(fixture->brightness_effective, 0);
	pids[0] = launcher->pid;
	fplinux_session_output_latched(fixture->session, 51, pids, 1);
	fplinux_session_output_presented(fixture->session, 51);
	expect_number(fixture->brightness_effective, 7);
}

static void test_keypad_light(struct fixture *fixture, bool binary)
{
	start_launcher(fixture);
	expect_number(fixture->keypad_brightness, binary ? 1 : 255);
	expect_file(fixture->keypad_trigger, "none");
	fixture->now_ms += 24000;
	pump(fixture, 4);
	expect_number(fixture->keypad_brightness, 0);
	phone_gesture(fixture, FPLINUX_KEY_2);
	expect_number(fixture->keypad_brightness, binary ? 1 : 255);
	fixture->now_ms += 30000;
	pump(fixture, 4);
	expect_number(fixture->keypad_brightness, 0);
	fplinux_session_destroy(fixture->session);
	fixture->session = NULL;
	expect_number(fixture->keypad_brightness, binary ? 1 : 37);
	expect_file(fixture->keypad_trigger, "timer");
}

static void test_settings_errors_time(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	struct fplinux_ui_session_message reply;

	setting(fixture, settings, "unknown", "x");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_ERROR,
			     "unknown");
	CHECK(!strcmp(reply.reason, "unknown-setting"));
	setting(fixture, settings, "screen_off_seconds", "17");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_ERROR,
			     "screen_off_seconds");
	CHECK(!strcmp(reply.reason, "invalid-value"));
	setting(fixture, settings, "screen_off_seconds", "");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
			     "screen_off_seconds");
	CHECK(!strcmp(reply.value, "30"));
	setting(fixture, settings, "brightness", "4");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_ERROR,
			     "brightness");
	CHECK(!strcmp(reply.reason, "unavailable"));
	setting(fixture, settings, "time", "");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
			     "time");
	CHECK(!strcmp(reply.value, "2025-01-01 00:00"));
	setting(fixture, settings, "time", "2025-02-30 12:34");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_ERROR,
			     "time");
	CHECK(!strcmp(reply.reason, "invalid-value"));
	CHECK(current_time == 1735689600);
	setting(fixture, settings, "time", "2025-02-28 12:34");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
			     "time");
	CHECK(!strcmp(reply.value, "2025-02-28 12:34"));
	CHECK(current_time == 1740746040);
	clock_fails = true;
	setting(fixture, settings, "time", "2025-03-01 01:02");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_ERROR,
			     "time");
	CHECK(!strcmp(reply.reason, "failed"));
	CHECK(current_time == 1740746040);
}

struct settings_session_context {
	struct fixture *fixture;
	struct peer *launcher;
	struct peer *settings;
	size_t request_index;
	const char *setting;
	const char *value;
	unsigned int rejected_changes;
	bool save_failed;
	enum fplinux_storage_state storage_state;
	enum fplinux_storage_state cancel_storage_state;
	int cancel_storage_error;
	unsigned int sync_calls_after_failure;
	uint32_t redraw_serial;
};

static void flush_settings_session(void *context, struct fplinux_ui *ui)
{
	struct settings_session_context *session = context;
	struct fplinux_ui_session_message message;
	bool progressed;

	do {
		progressed = false;
		while (settings_session_next_request(
			ui, &session->request_index, &message)) {
			issue(session->fixture, session->settings, &message);
			progressed = true;
		}
		pump(session->fixture, 10);
		while (session->settings->message_count) {
			message = session->settings->messages[0];
			--session->settings->message_count;
			memmove(session->settings->messages,
				&session->settings->messages[1],
				session->settings->message_count *
					sizeof(message));
			if (message.type == FPLINUX_UI_SESSION_ERROR &&
			    !strcmp(message.name, session->setting) &&
			    !strcmp(message.value, session->value)) {
				CHECK(!strcmp(message.reason,
					      session->save_failed ?
						      "save-failed" :
						      "not-foreground"));
				if (session->save_failed) {
					CHECK(message.applied);
					if (!strcmp(message.name, "theme") ||
					    !strcmp(message.name, "locale"))
						CHECK(message.serial);
					CHECK(message.storage_state ==
						      session->storage_state &&
					      message.storage_error == EIO);
					session->redraw_serial = message.serial;
					session->sync_calls_after_failure =
						sync_calls;
				}
				++session->rejected_changes;
			}
			if (session->sync_calls_after_failure &&
			    !strcmp(session->setting, "screen_off_seconds") &&
			    !strcmp(message.name, "brightness")) {
				CHECK(message.type ==
					      FPLINUX_UI_SESSION_SETTING &&
				      message.applied &&
				      !strcmp(message.value, "7"));
				session->cancel_storage_state =
					message.storage_state;
				session->cancel_storage_error =
					message.storage_error;
			}
			settings_session_receive(ui, &message);
			progressed = true;
		}
	} while (progressed);
}

static void recover_settings_storage(void *context)
{
	struct settings_session_context *session = context;

	CHECK(session->rejected_changes == 1);
	CHECK(sync_calls == session->sync_calls_after_failure);
	expect_file(session->fixture->brightness_level, "7\n");
	expect_file(
		session->fixture->settings,
		session->storage_state == FPLINUX_STORAGE_UNPUBLISHED ?
			"screen_off_seconds=30\nauto_lock_seconds=off\n"
			"locale=en\ninput_locale=en\ntheme=light\nbrightness=7\n" :
			"screen_off_seconds=60\nauto_lock_seconds=off\n"
			"locale=en\ninput_locale=en\ntheme=light\nbrightness=7\n");
	sync_fails = false;
	directory_sync_fails = false;
}

static void
test_settings_preview_cancel_keeps_storage_retry(struct fixture *fixture,
						 bool published)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	struct settings_session_context context = {
		.fixture = fixture,
		.launcher = launcher,
		.settings = settings,
		.setting = "screen_off_seconds",
		.value = "60",
		.save_failed = true,
		.storage_state = published ?
					 FPLINUX_STORAGE_PUBLISHED_UNSYNCED :
					 FPLINUX_STORAGE_UNPUBLISHED,
	};
	const struct settings_session_boundary boundary = {
		.flush = flush_settings_session,
		.recover_storage = recover_settings_storage,
		.context = &context,
	};

	sync_fails = !published;
	directory_sync_fails = published;
	CHECK(settings_session_preview_retry_test(&boundary) == 0);
	CHECK(context.rejected_changes == 1);
	CHECK(context.cancel_storage_state == context.storage_state);
	CHECK(context.cancel_storage_error == EIO);
	CHECK(sync_calls > context.sync_calls_after_failure);
	expect_file(fixture->settings,
		    "screen_off_seconds=60\nauto_lock_seconds=off\n"
		    "locale=en\ninput_locale=en\ntheme=light\nbrightness=7\n");
}

static void home_settings_session(void *context)
{
	struct settings_session_context *session = context;

	return_home(session->fixture, session->launcher);
	CHECK(fplinux_session_theme(session->fixture->session) ==
	      FPLINUX_UI_SESSION_THEME_LIGHT);
}

static void resume_settings_session(void *context)
{
	struct settings_session_context *session = context;
	struct fplinux_ui_session_message snapshot =
		query_tasks(session->fixture, session->launcher);
	const struct fplinux_ui_session_task *task =
		task_record(&snapshot, TEST_SETTINGS);

	CHECK(task->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	CHECK(session->rejected_changes == 1);
	CHECK(fplinux_session_theme(session->fixture->session) ==
	      FPLINUX_UI_SESSION_THEME_LIGHT);
	task_action(session->fixture, session->launcher,
		    FPLINUX_UI_SESSION_ACTIVATE, TEST_SETTINGS, task->token);
	struct fplinux_ui_session_message active =
		active_for(session->fixture, session->launcher, TEST_SETTINGS);

	CHECK(active.theme == FPLINUX_UI_SESSION_THEME_LIGHT);
	CHECK(!strcmp(active.locale, "en"));
	acknowledge(session->fixture, session->launcher, &active);
}

static void test_settings_choice_home_before_request(struct fixture *fixture,
						     bool theme)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	struct settings_session_context context = {
		.fixture = fixture,
		.launcher = launcher,
		.settings = settings,
		.setting = theme ? "theme" : "locale",
		.value = theme ? "dark" : "fr",
	};
	const struct settings_session_boundary boundary = {
		.flush = flush_settings_session,
		.home = home_settings_session,
		.resume = resume_settings_session,
		.context = &context,
	};

	CHECK(settings_session_test(theme, &boundary) == 0);
}

static void
test_settings_applied_error_rebuilds_before_ack(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	pid_t previous_launcher = launcher->pid;
	struct settings_session_context context = {
		.fixture = fixture,
		.launcher = launcher,
		.settings = settings,
		.setting = "theme",
		.value = "dark",
		.save_failed = true,
		.storage_state = FPLINUX_STORAGE_UNPUBLISHED,
	};
	const struct settings_session_boundary boundary = {
		.flush = flush_settings_session,
		.context = &context,
	};

	sync_fails = true;
	CHECK(settings_session_save_failure_test(&boundary) == 0);
	sync_fails = false;
	CHECK(context.rejected_changes == 1 && context.redraw_serial);
	CHECK(fixture->frame_held);
	struct fplinux_ui_session_message redraw = {
		.type = FPLINUX_UI_SESSION_REDRAW_ACK,
		.serial = context.redraw_serial,
	};

	issue(fixture, settings, &redraw);
	CHECK(fixture->frame_held);
	wait_reaped(fixture, previous_launcher);
	launcher = wait_peer(fixture, TEST_LAUNCHER);
	authenticate(fixture, launcher, NULL);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	fplinux_session_frame(fixture->session, launcher->pid);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_SETTINGS);

	acknowledge(fixture, launcher, &active);
	CHECK(!fixture->frame_held);
}

static void
test_fullscreen_launch_retains_until_first_buffer(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct fplinux_session_launch progress;

	add_entry(fixture, TEST_APP, "fullscreen");
	struct peer *app = launch_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message config =
		authenticate(fixture, app, NULL);

	CHECK(fixture->frame_held);
	fplinux_session_get_launch(fixture->session, &progress);
	CHECK(progress.active && !progress.shown);
	fixture->now_ms += 199;
	pump(fixture, 4);
	fplinux_session_get_launch(fixture->session, &progress);
	CHECK(fixture->frame_held && !progress.shown);
	++fixture->now_ms;
	pump(fixture, 4);
	fplinux_session_get_launch(fixture->session, &progress);
	CHECK(fixture->frame_held && progress.shown);
	commit_app(fixture, app, &config);
	CHECK(!fixture->frame_held);
	CHECK(fplinux_session_find_process(fixture->session, app->pid)->shown);
}

static void test_appearance_waits_for_both_frames(struct fixture *fixture,
						  bool redraw_first,
						  bool save_fails)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	pid_t previous_launcher = launcher->pid;
	struct fplinux_ui_session_message redraw = {
		.type = FPLINUX_UI_SESSION_REDRAW_ACK,
	};

	CHECK(!fixture->frame_held);
	sync_fails = save_fails;
	setting(fixture, settings, "theme", "dark");
	struct fplinux_ui_session_message reply =
		wait_message(fixture, settings,
			     save_fails ? FPLINUX_UI_SESSION_ERROR :
					  FPLINUX_UI_SESSION_SETTING,
			     "theme");

	sync_fails = false;
	CHECK(reply.serial && !strcmp(reply.value, "dark"));
	CHECK(reply.applied);
	CHECK(reply.storage_state == (save_fails ? FPLINUX_STORAGE_UNPUBLISHED :
						   FPLINUX_STORAGE_DURABLE));
	CHECK(reply.storage_error == (save_fails ? EIO : 0));
	CHECK(fixture->frame_held);
	redraw.serial = reply.serial + 1;
	issue(fixture, settings, &redraw);
	CHECK(fixture->frame_held);
	redraw.serial = reply.serial;
	if (redraw_first) {
		issue(fixture, settings, &redraw);
		CHECK(fixture->frame_held);
	}
	wait_reaped(fixture, previous_launcher);
	launcher = wait_peer(fixture, TEST_LAUNCHER);
	authenticate(fixture, launcher, NULL);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	fplinux_session_frame(fixture->session, launcher->pid);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_SETTINGS);

	CHECK(active.theme == FPLINUX_UI_SESSION_THEME_DARK);
	CHECK(fixture->frame_held);
	acknowledge(fixture, launcher, &active);
	if (!redraw_first) {
		CHECK(fixture->frame_held);
		issue(fixture, settings, &redraw);
	}
	CHECK(!fixture->frame_held);
	CHECK(fplinux_session_focused_pid(fixture->session) == settings->pid);
	setting(fixture, settings, "theme", "");
	reply = wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING,
			     "theme");
	CHECK(!reply.serial && !strcmp(reply.value, "dark"));
}

static void test_appearance_leave_retains_scene(struct fixture *fixture,
						bool retain)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	pid_t previous_launcher = launcher->pid;

	fixture->visible_frame = 1;
	setting(fixture, settings, "theme", "dark");
	wait_message(fixture, settings, FPLINUX_UI_SESSION_SETTING, "theme");
	CHECK(fixture->frame_held && fixture->retained_frame == 1);
	wait_reaped(fixture, previous_launcher);
	launcher = wait_peer(fixture, TEST_LAUNCHER);
	authenticate(fixture, launcher, NULL);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	fplinux_session_frame(fixture->session, launcher->pid);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_SETTINGS);

	acknowledge(fixture, launcher, &active);
	CHECK(fixture->frame_held);
	if (!retain)
		set_close_decision(fixture, settings, "");
	if (retain)
		CHECK(!phone_gesture(fixture, FPLINUX_KEY_POWER));
	else
		send_type(fixture, settings, FPLINUX_UI_SESSION_EXIT);
	if (!retain) {
		struct fplinux_ui_session_message prepare =
			wait_message(fixture, settings,
				     FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);
		const struct fplinux_session_process *process =
			fplinux_session_find_process(fixture->session,
						     settings->pid);

		CHECK(process && !process->closing &&
		      process->lifecycle == FPLINUX_UI_SESSION_FOREGROUND);
		answer_close(fixture, settings, prepare.serial, "ready");
	}
	active = active_for(fixture, launcher, "");
	CHECK(fixture->frame_held && fixture->retained_frame == 1);
	acknowledge(fixture, launcher, &active);
	CHECK(!fixture->frame_held);
}

static void
test_close_and_appearance_require_separate_frames(struct fixture *fixture)
{
	const uint16_t initial[] = { 0x1111, 0x2222, 0x3333, 0x4444 };
	const uint16_t decision[] = { 0xffff, 0x0000, 0xffe0, 0x0000 };
	struct peer *launcher = start_launcher(fixture);
	struct peer *settings = show_app(fixture, launcher, TEST_SETTINGS);
	pid_t previous_launcher = launcher->pid;

	present_scene(fixture, initial);
	set_close_decision(fixture, settings, "wait");
	send_type(fixture, settings, FPLINUX_UI_SESSION_EXIT);
	struct fplinux_ui_session_message prepare = wait_message(
		fixture, settings, FPLINUX_UI_SESSION_PREPARE_CLOSE, NULL);

	setting(fixture, settings, "theme", "dark");
	struct fplinux_ui_session_message reply = wait_message(
		fixture, settings, FPLINUX_UI_SESSION_SETTING, "theme");

	wait_reaped(fixture, previous_launcher);
	launcher = wait_peer(fixture, TEST_LAUNCHER);
	authenticate(fixture, launcher, NULL);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	fplinux_session_frame(fixture->session, launcher->pid);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_SETTINGS);

	acknowledge(fixture, launcher, &active);
	present_scene(fixture, decision);
	acknowledge_close(fixture, settings, prepare.serial);
	expect_scene(fixture, initial);
	struct fplinux_ui_session_message redraw = {
		.type = FPLINUX_UI_SESSION_REDRAW_ACK,
		.serial = reply.serial,
	};

	issue(fixture, settings, &redraw);
	expect_scene(fixture, decision);
}

static void test_surface_box(struct fixture *fixture, int top, int app_height)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = launch_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message config =
		authenticate(fixture, app, NULL);
	int x, y, width, height;

	CHECK(config.mode == FPLINUX_UI_SESSION_INTEGRATED);
	CHECK(config.screen_width == fixture->width &&
	      config.screen_height == fixture->height);
	fplinux_session_surface_box(fixture->session,
				    FPLINUX_UI_SESSION_INTEGRATED, &x, &y,
				    &width, &height);
	CHECK(x == 0 && y == top && width == fixture->width &&
	      height == app_height);
	fplinux_session_surface_box(fixture->session,
				    FPLINUX_UI_SESSION_FULLSCREEN, &x, &y,
				    &width, &height);
	CHECK(x == 0 && y == 0 && width == fixture->width &&
	      height == fixture->height);
}

static void test_battery_charger_wake(struct fixture *fixture,
				      const char *capacity, int percent,
				      const char *notice)
{
	struct peer *launcher = start_launcher(fixture);
	struct fplinux_ui_session_message active, next;

	clear_messages(fixture, launcher);
	write_file(fixture->charger_online, "1\n");
	fixture->now_ms += 2000;
	active = wait_message(fixture, launcher, FPLINUX_UI_SESSION_ACTIVE,
			      NULL);
	CHECK(active.charging && active.battery == 3 && !active.notice[0]);
	clear_messages(fixture, launcher);
	fixture->now_ms += 30000;
	active = wait_message(fixture, launcher, FPLINUX_UI_SESSION_ACTIVE,
			      NULL);
	CHECK(active.display == FPLINUX_UI_SESSION_DISPLAY_OFF);
	CHECK(active.charging && active.battery == 3 && !active.notice[0]);
	clear_messages(fixture, launcher);
	write_file(fixture->battery_capacity, capacity);
	fixture->now_ms += 2000;
	pump(fixture, 4);
	/* An idle charger poll must not refresh the battery while OFF. */
	CHECK(!take_message(launcher, FPLINUX_UI_SESSION_ACTIVE, NULL,
			    &active));
	write_file(fixture->charger_online, "0\n");
	fixture->now_ms += 2000;
	active = wait_message(fixture, launcher, FPLINUX_UI_SESSION_ACTIVE,
			      NULL);
	/* Include the next dispatch's fresh reading, not only the wake message. */
	pump(fixture, 4);
	while (take_message(launcher, FPLINUX_UI_SESSION_ACTIVE, NULL, &next))
		active = next;
	CHECK(active.display == FPLINUX_UI_SESSION_DISPLAY_ON);
	CHECK(!active.charging && active.battery == percent);
	CHECK(!strcmp(active.notice, notice));
}

static struct fplinux_ui_session_message battery_notice(struct fixture *fixture,
							struct peer *launcher,
							const char *capacity,
							const char *notice)
{
	write_file(fixture->battery_capacity, capacity);
	fixture->now_ms += 60000;
	struct fplinux_ui_session_message active = wait_message(
		fixture, launcher, FPLINUX_UI_SESSION_ACTIVE, NULL);
	struct fplinux_ui_session_message next;

	while (take_message(launcher, FPLINUX_UI_SESSION_ACTIVE, NULL, &next))
		active = next;
	CHECK(!strcmp(active.notice, notice));
	return active;
}

static void test_battery_notice_waits_for_current_frame(struct fixture *fixture,
							bool upgrade)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message keep_on = {
		.type = FPLINUX_UI_SESSION_KEEP_SCREEN_ON,
		.enabled = true,
	};
	struct fplinux_ui_session_message ignored;

	issue(fixture, app, &keep_on);
	clear_messages(fixture, launcher);
	clear_messages(fixture, app);
	struct fplinux_ui_session_message low =
		battery_notice(fixture, launcher, "10\n", "battery-low");
	struct fplinux_ui_session_message current = low;

	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	CHECK(fplinux_session_find_process(fixture->session, app->pid)->shown);
	CHECK(phone_gesture(fixture, FPLINUX_KEY_2));
	CHECK(!take_message(app, FPLINUX_UI_SESSION_VISIBILITY, NULL,
			    &ignored));
	commit_launcher(fixture, launcher);
	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	if (upgrade) {
		current = battery_notice(fixture, launcher, "3\n",
					 "battery-critical");
		acknowledge(fixture, launcher, &low);
		CHECK(!fplinux_session_launcher_on_top(fixture->session));
		CHECK(fplinux_session_focused_pid(fixture->session) ==
		      app->pid);
	} else {
		struct fplinux_ui_session_message stale = low;

		--stale.serial;
		acknowledge(fixture, launcher, &stale);
		CHECK(!fplinux_session_launcher_on_top(fixture->session));
	}
	acknowledge(fixture, launcher, &current);
	CHECK(fplinux_session_launcher_on_top(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	fplinux_session_set_launcher_shown(fixture->session, true);
	pump(fixture, 4);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
	ignored =
		wait_message(fixture, app, FPLINUX_UI_SESSION_VISIBILITY, NULL);
	CHECK(!ignored.visible);
	if (upgrade) {
		struct fplinux_ui_session_message dismiss = {
			.type = FPLINUX_UI_SESSION_DISMISS,
			.notice = "battery-low",
		};

		issue(fixture, launcher, &dismiss);
		CHECK(fplinux_session_launcher_on_top(fixture->session));
		CHECK(fplinux_session_focused_pid(fixture->session) ==
		      launcher->pid);
	}
	struct fplinux_ui_session_message dismiss = {
		.type = FPLINUX_UI_SESSION_DISMISS,
	};

	strcpy(dismiss.notice, current.notice);
	issue(fixture, launcher, &dismiss);
	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	fplinux_session_set_launcher_shown(fixture->session, false);
	pump(fixture, 4);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	ignored =
		wait_message(fixture, app, FPLINUX_UI_SESSION_VISIBILITY, NULL);
	CHECK(ignored.visible);
}

static volatile sig_atomic_t clock_signal_stopped;

static void clock_signal_stop(int number)
{
	clock_signal_stopped = number;
}

/* A real child replaces only the ALSA/evdev signal process in host tests. */
static int run_clock_signal_peer(void)
{
	const char *path = getenv("FPLINUX_TEST_CLOCK_SIGNAL_FILE");
	const char *mode = getenv("FPLINUX_TEST_CLOCK_SIGNAL_MODE");
	struct sigaction action = { .sa_handler = clock_signal_stop };
	FILE *file;

	CHECK(path && mode);
	sigemptyset(&action.sa_mask);
	CHECK(sigaction(SIGTERM, &action, NULL) == 0);
	file = fopen(path, "a");
	CHECK(file);
	CHECK(fprintf(file, "start %ld\n", (long)getpid()) > 0);
	CHECK(fclose(file) == 0);
	if (!strcmp(mode, "fail"))
		return 1;
	while (!clock_signal_stopped) {
		struct timespec interval = { .tv_nsec = 20000000 };

		nanosleep(&interval, NULL);
	}
	file = fopen(path, "a");
	CHECK(file);
	CHECK(fprintf(file, "stop %ld\n", (long)getpid()) > 0);
	CHECK(fclose(file) == 0);
	return 0;
}

static pid_t wait_clock_signal(struct fixture *fixture, unsigned int starts,
			       unsigned int stops)
{
	uint64_t deadline = real_ms() + 2000;
	const char *path = getenv("FPLINUX_TEST_CLOCK_SIGNAL_FILE");
	pid_t last_pid = 0;

	do {
		FILE *file = fopen(path, "r");
		unsigned int started = 0, stopped = 0;
		char kind[16];
		long pid;

		if (file) {
			while (fscanf(file, "%15s %ld", kind, &pid) == 2) {
				if (!strcmp(kind, "start")) {
					++started;
					last_pid = (pid_t)pid;
				} else {
					CHECK(!strcmp(kind, "stop"));
					++stopped;
				}
			}
			CHECK(!ferror(file));
			CHECK(fclose(file) == 0);
		}
		if (started == starts && stopped == stops)
			return last_pid;
		pump(fixture, 2);
	} while (real_ms() < deadline);
	CHECK(false);
	return 0;
}

static struct fplinux_ui_session_message
clock_request(struct fixture *fixture, struct peer *peer, const char *name,
	      const char *json, uint32_t serial)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = serial,
	};

	CHECK(strlen(name) < sizeof(message.name));
	CHECK(strlen(json) < sizeof(message.clock_data));
	strcpy(message.name, name);
	strcpy(message.clock_data, json);
	issue(fixture, peer, &message);
	message = wait_message(fixture, peer, FPLINUX_UI_SESSION_CLOCK, name);
	CHECK(message.serial == serial);
	return message;
}

static struct clock_model clock_snapshot(struct fixture *fixture,
					 struct peer *peer, uint32_t serial)
{
	struct fplinux_ui_session_message reply = clock_request(
		fixture, peer, "snapshot", "{\"op\":\"snapshot\"}", serial);
	struct clock_model model;
	struct clock_result result;
	uint64_t response_serial;

	CHECK(!reply.reason[0]);
	CHECK(clock_wire_snapshot_decode(
		      reply.clock_data, strlen(reply.clock_data), &model,
		      &result, &response_serial) == CLOCK_OK);
	CHECK(result.status == CLOCK_OK && response_serial == serial);
	return model;
}

static struct fplinux_ui_session_message
clock_active(struct fixture *fixture, struct peer *launcher,
	     enum clock_event_kind kind, struct clock_ring *ring)
{
	struct fplinux_ui_session_message message;
	uint64_t serial;

	do {
		message = wait_message(fixture, launcher,
				       FPLINUX_UI_SESSION_ACTIVE, NULL);
		CHECK(clock_wire_ring_decode(message.clock_data,
					     strlen(message.clock_data), ring,
					     &serial) == CLOCK_OK);
	} while (ring->event.kind != kind);
	CHECK(serial == ring->event.id);
	return message;
}

static void test_clock_survives_home_close(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *clocks = show_app(fixture, launcher, TEST_CLOCK);
	struct fplinux_ui_session_message reply =
		clock_request(fixture, clocks, "timer-set",
			      "{\"op\":\"timer-set\",\"seconds\":5}", 1);

	CHECK(!reply.reason[0]);
	reply = clock_request(fixture, clocks, "timer-start",
			      "{\"op\":\"timer-start\"}", 2);
	CHECK(!reply.reason[0]);
	reply = clock_request(fixture, clocks, "stopwatch-start",
			      "{\"op\":\"stopwatch-start\"}", 3);
	CHECK(!reply.reason[0]);
	struct clock_model snapshot = clock_snapshot(fixture, clocks, 4);

	CHECK(snapshot.timer.state == CLOCK_TIMER_RUNNING);
	CHECK(snapshot.timer.deadline_ms == 6000);
	CHECK(snapshot.stopwatch.running);
	return_home(fixture, launcher);
	struct fplinux_ui_session_message background = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = 5,
		.name = "timer-stop",
		.clock_data = "{\"op\":\"timer-stop\"}",
	};

	issue(fixture, clocks, &background);
	CHECK(!take_message(clocks, FPLINUX_UI_SESSION_CLOCK, NULL, &reply));
	struct fplinux_ui_session_message tasks =
		query_tasks(fixture, launcher);
	uint32_t clock_token = task_record(&tasks, TEST_CLOCK)->token;

	task_action(fixture, launcher, FPLINUX_UI_SESSION_ACTIVATE, TEST_CLOCK,
		    clock_token);
	struct fplinux_ui_session_message resumed =
		active_for(fixture, launcher, TEST_CLOCK);

	acknowledge(fixture, launcher, &resumed);
	CHECK(fplinux_session_focused_pid(fixture->session) == clocks->pid);
	snapshot = clock_snapshot(fixture, clocks, 6);
	CHECK(snapshot.timer.state == CLOCK_TIMER_RUNNING);
	send_type(fixture, clocks, FPLINUX_UI_SESSION_EXIT);
	struct fplinux_ui_session_message home =
		active_for(fixture, launcher, "");

	acknowledge(fixture, launcher, &home);
	wait_reaped(fixture, clocks->pid);
	struct peer *notes = show_app(fixture, launcher, TEST_APP);
	struct clock_ring ring;

	clear_messages(fixture, launcher);
	fixture->now_ms = 7000;
	current_time -= 3600;
	pump(fixture, 4);
	struct fplinux_ui_session_message active =
		clock_active(fixture, launcher, CLOCK_EVENT_TIMER, &ring);

	CHECK(ring.event.id == 1 && ring.event.source_id == 0);
	CHECK(ring.event.due_monotonic_ms == 6000);
	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == notes->pid);
	acknowledge(fixture, launcher, &active);
	CHECK(fplinux_session_launcher_on_top(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == notes->pid);
	fplinux_session_set_launcher_shown(fixture->session, true);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
	reply = clock_request(fixture, launcher, "dismiss",
			      "{\"op\":\"dismiss\",\"event_id\":1}", 7);
	CHECK(!reply.reason[0]);
	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	fplinux_session_set_launcher_shown(fixture->session, false);
	CHECK(fplinux_session_focused_pid(fixture->session) == notes->pid);
	return_home(fixture, launcher);
	clocks = show_app(fixture, launcher, TEST_CLOCK);
	snapshot = clock_snapshot(fixture, clocks, 8);
	CHECK(snapshot.timer.state == CLOCK_TIMER_FINISHED);
	CHECK(snapshot.stopwatch.running);
	CHECK(clock_model_stopwatch_elapsed(&snapshot) == 6000);
}

static void test_clock_authority(struct fixture *fixture, bool launcher_only)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *peer = launcher_only ?
				    launcher :
				    show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message request = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = 17,
		.name = "timer-set",
		.app_id = TEST_CLOCK,
		.clock_data = "{\"op\":\"timer-set\",\"seconds\":1}",
	};
	issue(fixture, peer, &request);
	/* Rejection closes the managed process; it may exit before forwarding
	 * a disconnect notification through the test-only observation channel.
	 */
	wait_reaped(fixture, peer->pid);
	CHECK(!take_message(peer, FPLINUX_UI_SESSION_CLOCK, NULL, &request));
	if (launcher_only) {
		launcher = wait_peer(fixture, TEST_LAUNCHER);
		authenticate(fixture, launcher, NULL);
		send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
		fplinux_session_surface_committed(fixture->session,
						  launcher->pid, fixture->width,
						  fixture->height);
		fplinux_session_frame(fixture->session, launcher->pid);
		struct fplinux_ui_session_message active =
			active_for(fixture, launcher, "");

		acknowledge(fixture, launcher, &active);
	}
	struct peer *clocks = show_app(fixture, launcher, TEST_CLOCK);
	struct clock_model model = clock_snapshot(fixture, clocks, 18);

	CHECK(model.timer.state == CLOCK_TIMER_STOPPED);
	CHECK(model.timer.duration_seconds == 0);
	fixture->now_ms += 2000;
	pump(fixture, 4);
	model = clock_snapshot(fixture, clocks, 19);
	CHECK(model.active.kind == CLOCK_EVENT_NONE);
	CHECK(model.timer.state == CLOCK_TIMER_STOPPED);
}

static void test_clock_save_retry_preserves_identity(struct fixture *fixture,
						     bool published)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *clocks = show_app(fixture, launcher, TEST_CLOCK);
	const char *add =
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"Wake\",\"schedule\":0,"
		"\"once_utc\":1735689660,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}";
	const char *same_add =
		"{ \"alarm\":{\"enabled\":true,\"repeat_days\":0,\"minute\":0,\"hour\":0,"
		"\"once_utc\":1735689660,\"schedule\":0,\"label\":\"Wake\"},\"op\":\"alarm-add\"}";
	struct fplinux_ui_session_message reply;
	struct clock_model model;
	struct clock_result result;
	uint64_t serial;

	sync_fails = !published;
	directory_sync_fails = published;
	reply = clock_request(fixture, clocks, "alarm-add", add, 11);
	CHECK(!strcmp(reply.reason, "storage-error"));
	CHECK(clock_wire_snapshot_decode(reply.clock_data,
					 strlen(reply.clock_data), &model,
					 &result, &serial) == CLOCK_OK);
	CHECK(result.applied == published && result.storage.error == EIO);
	CHECK(result.storage.state ==
	      (published ? FPLINUX_STORAGE_PUBLISHED_UNSYNCED :
			   FPLINUX_STORAGE_UNPUBLISHED));
	CHECK(model.alarm_count == (published ? 1 : 0));
	CHECK(model.next_alarm_id == (published ? 2 : 1));
	sync_fails = false;
	directory_sync_fails = false;
	model = clock_snapshot(fixture, clocks, 12);
	CHECK(model.alarm_count == (published ? 1 : 0));
	CHECK(model.next_alarm_id == (published ? 2 : 1));
	reply = clock_request(fixture, clocks, "alarm-add", add, 11);
	CHECK(!reply.reason[0]);
	/* A successful reply may be lost; the same semantic request is retried. */
	reply = clock_request(fixture, clocks, "alarm-add", same_add, 11);
	CHECK(!reply.reason[0]);
	model = clock_snapshot(fixture, clocks, 13);
	CHECK(model.alarm_count == 1 && model.alarms[0].id == 1);
	CHECK(model.next_alarm_id == 2);
	reply = clock_request(fixture, clocks, "timer-set",
			      "{\"op\":\"timer-set\",\"seconds\":90}", 11);
	CHECK(!strcmp(reply.reason, "invalid"));
	model = clock_snapshot(fixture, clocks, 14);
	CHECK(model.alarm_count == 1 && model.timer.duration_seconds == 0);
}

static void
test_clock_save_checkpoints_current_model_without_edit(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *clocks = show_app(fixture, launcher, TEST_CLOCK);
	struct clock_model model;
	struct clock_result result;
	uint64_t serial;

	directory_sync_fails = true;
	struct fplinux_ui_session_message reply = clock_request(
		fixture, clocks, "alarm-add",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"Wake\",\"schedule\":0,"
		"\"once_utc\":1735689660,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
		11);

	CHECK(!strcmp(reply.reason, "storage-error"));
	reply = clock_request(fixture, clocks, "stopwatch-start",
			      "{\"op\":\"stopwatch-start\"}", 12);
	CHECK(!reply.reason[0]);
	current_time += 60;
	fixture->now_ms += 60000;
	pump(fixture, 4);
	directory_sync_fails = false;
	reply = clock_request(fixture, clocks, "save", "{\"op\":\"save\"}", 13);
	CHECK(!reply.reason[0]);
	CHECK(clock_wire_snapshot_decode(reply.clock_data,
					 strlen(reply.clock_data), &model,
					 &result, &serial) == CLOCK_OK);
	CHECK(result.applied && result.status == CLOCK_OK && serial == 13);
	CHECK(result.storage.state == FPLINUX_STORAGE_DURABLE &&
	      !result.storage.error);
	CHECK(model.alarm_count == 1 && model.alarms[0].id == 1);
	CHECK(model.alarms[0].generation == 1 && model.next_alarm_id == 2);
	CHECK(model.active.id == 1 && model.active.generation == 1);
	CHECK(model.stopwatch.running && model.stopwatch.elapsed_ms == 60000);
}

static void test_clock_locked_ring_keys(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *clocks = show_app(fixture, launcher, TEST_CLOCK);
	struct fplinux_ui_session_message reply = clock_request(
		fixture, clocks, "alarm-add",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"Wake\",\"schedule\":0,"
		"\"once_utc\":1735689660,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
		1);

	CHECK(!reply.reason[0]);
	return_home(fixture, launcher);
	struct fplinux_ui_session_message select = {
		.type = FPLINUX_UI_SESSION_SELECT,
		.value = "home",
	};

	issue(fixture, launcher, &select);
	phone_gesture(fixture, FPLINUX_KEY_POWER);
	CHECK(fplinux_session_is_locked(fixture->session));
	current_time += 60;
	fixture->now_ms += 60000;
	pump(fixture, 4);
	struct clock_ring ring;
	struct fplinux_ui_session_message active =
		clock_active(fixture, launcher, CLOCK_EVENT_ALARM, &ring);

	CHECK(ring.event.id == 1 && !strcmp(ring.event.label, "Wake"));
	CHECK(active.locked && active.display == FPLINUX_UI_SESSION_DISPLAY_ON);
	CHECK(fplinux_session_wake_pending(fixture->session));
	acknowledge(fixture, launcher, &active);
	fplinux_session_set_launcher_shown(fixture->session, true);
	CHECK(fplinux_session_keyboard_key(fixture->session, KEY_COMPOSE,
					   true));
	CHECK(fplinux_session_keyboard_key(fixture->session, KEY_COMPOSE,
					   false));
	CHECK(fplinux_session_is_locked(fixture->session));
	CHECK(fplinux_session_key(fixture->session, FPLINUX_KEY_SOFT_LEFT, true,
				  fixture->now_ms));
	reply = clock_request(
		fixture, launcher, "snooze",
		"{\"op\":\"snooze\",\"event_id\":1,\"minutes\":1}", 2);
	CHECK(!reply.reason[0]);
	CHECK(!fplinux_session_key(fixture->session, FPLINUX_KEY_SOFT_LEFT,
				   false, fixture->now_ms + 1));
	CHECK(fplinux_session_is_locked(fixture->session));
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_STAR));
	clear_messages(fixture, launcher);
	fixture->now_ms += 60000;
	pump(fixture, 4);
	active = clock_active(fixture, launcher, CLOCK_EVENT_ALARM, &ring);
	CHECK(ring.event.id == 2 && ring.event.snoozed);
	acknowledge(fixture, launcher, &active);
	CHECK(!fplinux_session_key(fixture->session, FPLINUX_KEY_SOFT_RIGHT,
				   false, fixture->now_ms));
	CHECK(fplinux_session_key(fixture->session, FPLINUX_KEY_SOFT_RIGHT,
				  true, fixture->now_ms));
	CHECK(fplinux_session_key(fixture->session, FPLINUX_KEY_SOFT_RIGHT,
				  false, fixture->now_ms + 1));
	reply = clock_request(fixture, launcher, "dismiss",
			      "{\"op\":\"dismiss\",\"event_id\":1}", 3);
	CHECK(!strcmp(reply.reason, "stale-event"));
	reply = clock_request(fixture, launcher, "dismiss",
			      "{\"op\":\"dismiss\",\"event_id\":2}", 4);
	CHECK(!reply.reason[0]);
	CHECK(fplinux_session_is_locked(fixture->session));
}

static void test_clock_recovers_after_reset_rtc(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *clocks = show_app(fixture, launcher, TEST_CLOCK);
	struct fplinux_ui_session_message reply = clock_request(
		fixture, clocks, "snapshot", "{\"op\":\"snapshot\"}", 1);

	CHECK(!strcmp(reply.reason, "clock-unavailable"));
	CHECK(!reply.clock_data[0]);
	CHECK(fplinux_session_focused_pid(fixture->session) == clocks->pid);
	current_time = 1735689600;
	pump(fixture, 4);
	struct clock_model model = clock_snapshot(fixture, clocks, 2);

	CHECK(model.wall_utc == 1735689600 && model.alarm_count == 0);
	reply = clock_request(fixture, clocks, "timer-set",
			      "{\"op\":\"timer-set\",\"seconds\":2}", 3);
	CHECK(!reply.reason[0]);
	reply = clock_request(fixture, clocks, "timer-start",
			      "{\"op\":\"timer-start\"}", 4);
	CHECK(!reply.reason[0]);
	fixture->now_ms += 2000;
	pump(fixture, 4);
	struct clock_ring ring;

	clock_active(fixture, launcher, CLOCK_EVENT_TIMER, &ring);
	CHECK(ring.event.id == 1 && ring.event.due_monotonic_ms == 3000);
}

static void test_clock_restart_needs_new_ring_frame(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *clocks = show_app(fixture, launcher, TEST_CLOCK);
	struct command crash = { .type = COMMAND_CRASH };
	struct clock_ring ring;

	clock_request(fixture, clocks, "timer-set",
		      "{\"op\":\"timer-set\",\"seconds\":1}", 1);
	clock_request(fixture, clocks, "timer-start",
		      "{\"op\":\"timer-start\"}", 2);
	fixture->now_ms += 1000;
	pump(fixture, 4);
	struct fplinux_ui_session_message active =
		clock_active(fixture, launcher, CLOCK_EVENT_TIMER, &ring);

	acknowledge(fixture, launcher, &active);
	fplinux_session_set_launcher_shown(fixture->session, true);
	CHECK(fplinux_session_launcher_on_top(fixture->session));
	send_command(launcher->fd, &crash);
	wait_reaped(fixture, launcher->pid);
	launcher = wait_peer(fixture, TEST_LAUNCHER);
	authenticate(fixture, launcher, NULL);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	fplinux_session_frame(fixture->session, launcher->pid);
	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == clocks->pid);
	active = clock_active(fixture, launcher, CLOCK_EVENT_TIMER, &ring);
	CHECK(ring.event.id == 1);
	acknowledge(fixture, launcher, &active);
	CHECK(fplinux_session_launcher_on_top(fixture->session));
	fplinux_session_set_launcher_shown(fixture->session, true);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
}

static unsigned int audio_count(struct fixture *fixture, const char *kind,
				pid_t pid)
{
	char prefix[64];
	unsigned int count = 0;
	size_t length;

	CHECK(snprintf(prefix, sizeof(prefix), "%s %ld", kind, (long)pid) <
	      (int)sizeof(prefix));
	length = strlen(prefix);
	for (size_t i = 0; i < fixture->audio_message_count; ++i)
		if (!strncmp(fixture->audio_messages[i], prefix, length) &&
		    (fixture->audio_messages[i][length] == '\0' ||
		     fixture->audio_messages[i][length] == ' '))
			++count;
	return count;
}

static int audio_audible(struct fixture *fixture, pid_t pid)
{
	int value = -1;

	for (size_t i = 0; i < fixture->audio_message_count; ++i) {
		long owner;
		int audible;

		if (sscanf(fixture->audio_messages[i], "AUDIBLE %ld %d", &owner,
			   &audible) == 2 &&
		    owner == (long)pid)
			value = audible;
	}
	return value;
}

static void start_vibration(struct fixture *fixture, struct peer *app)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_VIBRATE,
		.pattern_count = 3,
		.pattern = { 0, 1000, 1000 },
		.repeat = true,
	};

	/* A second request observes the motor's minimum gap after the last stop. */
	fixture->now_ms += 100;
	issue(fixture, app, &message);
	CHECK(fixture->vibrator_on);
}

static void test_media_home_resume_close(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	pid_t pid = app->pid;
	struct fplinux_ui_session_message active;

	pump(fixture, 4);
	CHECK(audio_count(fixture, "APP", pid) == 1 &&
	      audio_audible(fixture, pid) == 1);
	start_vibration(fixture, app);
	clear_messages(fixture, launcher);
	fplinux_session_home(fixture->session);
	active = active_for(fixture, launcher, "");
	acknowledge(fixture, launcher, &active);
	pump(fixture, 4);
	CHECK(!fixture->vibrator_on && audio_audible(fixture, pid) == 0);
	CHECK(audio_count(fixture, "GONE", pid) == 0);
	CHECK(launch_app(fixture, launcher, TEST_APP)->pid == pid);
	active = active_for(fixture, launcher, TEST_APP);
	acknowledge(fixture, launcher, &active);
	pump(fixture, 4);
	CHECK(audio_audible(fixture, pid) == 1 && !fixture->vibrator_on);
	CHECK(audio_count(fixture, "APP", pid) == 1);
	start_vibration(fixture, app);
	clear_messages(fixture, launcher);
	send_type(fixture, app, FPLINUX_UI_SESSION_EXIT);
	active = active_for(fixture, launcher, "");
	acknowledge(fixture, launcher, &active);
	wait_message(fixture, app, FPLINUX_UI_SESSION_CLOSE, NULL);
	wait_reaped(fixture, pid);
	CHECK(!fixture->vibrator_on && audio_count(fixture, "GONE", pid) == 1);
}

static void test_media_lock_crash(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct command crash = { .type = COMMAND_CRASH };
	pid_t pid = app->pid;
	struct fplinux_ui_session_message request = {
		.type = FPLINUX_UI_SESSION_VIBRATE,
		.pattern_count = 2,
		.pattern = { 0, 1000 },
	};

	start_vibration(fixture, app);
	fixture->now_ms += 30000;
	pump(fixture, 4);
	CHECK(fplinux_session_is_locked(fixture->session));
	CHECK(!fixture->vibrator_on && audio_audible(fixture, pid) == 0);
	CHECK(audio_count(fixture, "GONE", pid) == 0);
	issue(fixture, app, &request);
	struct fplinux_ui_session_message error = wait_message(
		fixture, app, FPLINUX_UI_SESSION_ERROR, "vibration");

	CHECK(!strcmp(error.reason, strerror(EAGAIN)) && !fixture->vibrator_on);
	send_command(app->fd, &crash);
	wait_reaped(fixture, pid);
	CHECK(!fixture->vibrator_on && audio_count(fixture, "GONE", pid) == 1);
}

static void test_media_effect_error(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message request = {
		.type = FPLINUX_UI_SESSION_VIBRATE,
		.pattern_count = 2,
		.pattern = { 0, 200 },
	};

	fixture->fail_vibration = true;
	issue(fixture, app, &request);
	struct fplinux_ui_session_message error = wait_message(
		fixture, app, FPLINUX_UI_SESSION_ERROR, "vibration");

	CHECK(!strcmp(error.reason, strerror(EIO)) && !fixture->vibrator_on);
	CHECK(audio_audible(fixture, app->pid) == 1);
	issue(fixture, app, &request);
	error = wait_message(fixture, app, FPLINUX_UI_SESSION_ERROR,
			     "vibration");
	CHECK(!strcmp(error.reason, strerror(ENODEV)) && !fixture->vibrator_on);
}

static void test_clock_signal_lifetime(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *clocks = show_app(fixture, launcher, TEST_CLOCK);
	struct fplinux_ui_session_message reply =
		clock_request(fixture, clocks, "timer-set",
			      "{\"op\":\"timer-set\",\"seconds\":1}", 1);

	CHECK(!reply.reason[0]);
	clock_request(fixture, clocks, "timer-start",
		      "{\"op\":\"timer-start\"}", 2);
	fixture->now_ms += 1000;
	pid_t failed_pid = wait_clock_signal(fixture, 1, 0);

	pump(fixture, 20);
	if (fixture->media) {
		CHECK(audio_count(fixture, "CLOCK", failed_pid) == 1);
		CHECK(audio_audible(fixture, failed_pid) == 1);
		CHECK(audio_count(fixture, "GONE", failed_pid) == 1);
	}
	errno = 0;
	CHECK(waitpid(failed_pid, NULL, WNOHANG) == -1 && errno == ECHILD);
	clock_request(fixture, launcher, "dismiss",
		      "{\"op\":\"dismiss\",\"event_id\":1}", 3);
	CHECK(setenv("FPLINUX_TEST_CLOCK_SIGNAL_MODE", "wait", 1) == 0);
	clock_request(fixture, clocks, "timer-restart",
		      "{\"op\":\"timer-restart\"}", 4);
	fixture->now_ms += 1000;
	pid_t second_pid = wait_clock_signal(fixture, 2, 0);

	CHECK(second_pid != failed_pid);
	if (fixture->media) {
		CHECK(audio_count(fixture, "CLOCK", second_pid) == 1);
		CHECK(audio_audible(fixture, second_pid) == 1);
		CHECK(audio_count(fixture, "GONE", second_pid) == 0);
	}
	clock_request(fixture, launcher, "dismiss",
		      "{\"op\":\"dismiss\",\"event_id\":2}", 5);
	wait_clock_signal(fixture, 2, 1);
	pump(fixture, 20);
	if (fixture->media)
		CHECK(audio_count(fixture, "GONE", second_pid) == 1);
	errno = 0;
	CHECK(waitpid(second_pid, NULL, WNOHANG) == -1 && errno == ECHILD);
	clock_request(fixture, clocks, "timer-restart",
		      "{\"op\":\"timer-restart\"}", 6);
	fixture->now_ms += 1000;
	pid_t final_pid = wait_clock_signal(fixture, 3, 1);

	fplinux_session_destroy(fixture->session);
	fixture->session = NULL;
	if (fixture->media) {
		observe_audio(fixture);
		CHECK(audio_count(fixture, "CLOCK", final_pid) == 1);
		CHECK(audio_count(fixture, "GONE", final_pid) == 1);
		CHECK(audio_count(fixture, "GONE", clocks->pid) == 1);
	}
	wait_clock_signal(fixture, 3, 2);
	errno = 0;
	CHECK(waitpid(final_pid, NULL, WNOHANG) == -1 && errno == ECHILD);
}

int main(int argc, char **argv)
{
	if (getenv("FPLINUX_APP_ID"))
		return run_peer();
	if (argc == 1 && getenv("FPLINUX_TEST_CLOCK_SIGNAL_FILE"))
		return run_clock_signal_peer();
	CHECK(argc == 3 || argc == 7);
	CHECK(atexit(cleanup) == 0);
	bool brightness = !strcmp(argv[1], "brightness-home") ||
			  !strcmp(argv[1], "brightness-crash") ||
			  !strncmp(argv[1], "settings-preview-cancel-", 24) ||
			  !strncmp(argv[1], "task-view-", 10) ||
			  !strncmp(argv[1], "wake-", 5);
	unsigned int keypad_max = 0;
	int width = argc == 7 ? atoi(argv[3]) : 240;
	int height = argc == 7 ? atoi(argv[4]) : 320;
	struct fixture *fixture;
	char signal_path[PATH_MAX];

	if (!strcmp(argv[1], "clock-reset-rtc"))
		current_time = 0;
	if (!strcmp(argv[1], "clock-signal-lifetime") ||
	    !strcmp(argv[1], "media-clock-owner")) {
		path_join(signal_path, argv[2], "signal-log");
		CHECK(setenv("FPLINUX_TEST_CLOCK_SIGNAL_FILE", signal_path,
			     1) == 0);
		CHECK(setenv("FPLINUX_TEST_CLOCK_SIGNAL_MODE", "fail", 1) == 0);
	}

	if (!strcmp(argv[1], "keypad-light"))
		keypad_max = 255;
	else if (!strcmp(argv[1], "keypad-light-binary"))
		keypad_max = 1;
	fixture = create_fixture(
		argv[2], width, height, brightness, keypad_max,
		!strncmp(argv[1], "task-localized-", 15) ? "ru" : "en",
		!strncmp(argv[1], "battery-notice-", 15) ? 80 :
		!strncmp(argv[1], "battery-", 8)	 ? 3 :
							   -1,
		!strncmp(argv[1], "media-", 6));

	if (!strcmp(argv[1], "media-home-resume-close"))
		test_media_home_resume_close(fixture);
	else if (!strcmp(argv[1], "media-lock-crash"))
		test_media_lock_crash(fixture);
	else if (!strcmp(argv[1], "media-effect-error"))
		test_media_effect_error(fixture);
	else if (!strcmp(argv[1], "media-clock-owner"))
		test_clock_signal_lifetime(fixture);
	else if (!strcmp(argv[1], "clock-reset-rtc"))
		test_clock_recovers_after_reset_rtc(fixture);
	else if (!strcmp(argv[1], "clock-signal-lifetime"))
		test_clock_signal_lifetime(fixture);
	else if (!strcmp(argv[1], "clock-launcher-restart"))
		test_clock_restart_needs_new_ring_frame(fixture);
	else if (!strcmp(argv[1], "clock-survives-home-close"))
		test_clock_survives_home_close(fixture);
	else if (!strcmp(argv[1], "clock-foreign-peer"))
		test_clock_authority(fixture, false);
	else if (!strcmp(argv[1], "clock-launcher-authority"))
		test_clock_authority(fixture, true);
	else if (!strcmp(argv[1], "clock-locked-ring-keys"))
		test_clock_locked_ring_keys(fixture);
	else if (!strcmp(argv[1], "clock-save-retry"))
		test_clock_save_retry_preserves_identity(fixture, false);
	else if (!strcmp(argv[1], "clock-published-retry"))
		test_clock_save_retry_preserves_identity(fixture, true);
	else if (!strcmp(argv[1], "clock-save-current-model"))
		test_clock_save_checkpoints_current_model_without_edit(fixture);
	else if (!strcmp(argv[1], "startup-frame-before-ready"))
		test_startup_frame_before_ready(fixture);
	else if (!strcmp(argv[1], "home-retains-exit-saves"))
		test_home_retains_exit_saves(fixture);
	else if (!strcmp(argv[1], "close-cancel-stale"))
		test_close_cancel_and_stale_reply(fixture);
	else if (!strcmp(argv[1], "close-ready-scenes"))
		test_close_ready_retains_scene_before_reply(fixture);
	else if (!strcmp(argv[1], "close-wait-scenes"))
		test_close_wait_needs_decision_and_system_frames(fixture);
	else if (!strcmp(argv[1], "close-wait-home-repeat"))
		test_close_wait_survives_home_and_repeated_close(fixture);
	else if (!strcmp(argv[1], "close-unanswered-timeout"))
		test_close_unanswered_has_bounded_timeout(fixture);
	else if (!strcmp(argv[1], "close-wait-after-home"))
		test_delayed_close_wait_after_home_stays_hidden(fixture);
	else if (!strcmp(argv[1], "close-background-wait"))
		test_close_background_wait_activates_same_instance(fixture);
	else if (!strcmp(argv[1], "settings-retry-latest"))
		test_settings_retries_latest_value(fixture, false);
	else if (!strcmp(argv[1], "settings-retry-unchanged"))
		test_settings_retries_latest_value(fixture, true);
	else if (!strcmp(argv[1], "settings-applied-error-view"))
		test_settings_applied_error_rebuilds_before_ack(fixture);
	else if (!strcmp(argv[1], "settings-preview-cancel-retry"))
		test_settings_preview_cancel_keeps_storage_retry(fixture,
								 false);
	else if (!strcmp(argv[1], "settings-preview-cancel-published-retry"))
		test_settings_preview_cancel_keeps_storage_retry(fixture, true);
	else if (!strcmp(argv[1], "shutdown-saves-bounded"))
		test_shutdown_saves_and_bounds_unresponsive_peers(fixture);
	else if (!strcmp(argv[1], "task-controls"))
		test_task_controls(fixture);
	else if (!strcmp(argv[1], "task-localized-title"))
		test_task_localized_title(fixture);
	else if (!strcmp(argv[1], "activation-intent-frame-barrier"))
		test_activation_intent_and_frame_barrier(fixture);
	else if (!strcmp(argv[1], "activation-ended-task"))
		test_resume_ended_task_during_handoff(fixture);
	else if (!strcmp(argv[1], "activation-cancel-retains"))
		test_retained_activation_cancel(fixture, "cancel");
	else if (!strcmp(argv[1], "activation-home-retains"))
		test_retained_activation_cancel(fixture, "home");
	else if (!strcmp(argv[1], "activation-timeout-retains"))
		test_retained_activation_cancel(fixture, "timeout");
	else if (!strcmp(argv[1], "task-localized-live"))
		test_task_titles_follow_applied_locale(fixture, false, false);
	else if (!strcmp(argv[1], "task-localized-live-save-fails"))
		test_task_titles_follow_applied_locale(fixture, false, true);
	else if (!strcmp(argv[1], "task-localized-explicit-default"))
		test_task_titles_follow_applied_locale(fixture, true, false);
	else if (!strcmp(argv[1], "task-stale-lock"))
		test_task_stale_and_locked(fixture);
	else if (!strcmp(argv[1], "red-home-retains"))
		test_red_home_retains_task(fixture, false);
	else if (!strcmp(argv[1], "red-home-fullscreen"))
		test_red_home_retains_task(fixture, true);
	else if (!strcmp(argv[1], "task-view-red-home"))
		test_task_view_red_returns_home(fixture);
	else if (!strcmp(argv[1], "task-view-back-home"))
		test_task_view_closed_home_locks(fixture, true);
	else if (!strcmp(argv[1], "task-view-query-home"))
		test_task_view_closed_home_locks(fixture, false);
	else if (!strcmp(argv[1], "task-view-restart-home"))
		test_task_view_restart_restores_base(fixture, false);
	else if (!strcmp(argv[1], "task-view-restart-catalogue"))
		test_task_view_restart_restores_base(fixture, true);
	else if (!strcmp(argv[1], "task-view-activation-selection"))
		test_task_view_close_preserves_activated_selection(fixture);
	else if (!strcmp(argv[1], "first-buffer-handoff"))
		test_first_buffer_handoff(fixture, true);
	else if (!strcmp(argv[1], "ready-after-buffer"))
		test_first_buffer_handoff(fixture, false);
	else if (!strcmp(argv[1], "fullscreen-launch-held"))
		test_fullscreen_launch_retains_until_first_buffer(fixture);
	else if (!strcmp(argv[1], "appearance-launcher-first"))
		test_appearance_waits_for_both_frames(fixture, false, false);
	else if (!strcmp(argv[1], "appearance-requester-first"))
		test_appearance_waits_for_both_frames(fixture, true, false);
	else if (!strcmp(argv[1], "appearance-save-fails"))
		test_appearance_waits_for_both_frames(fixture, false, true);
	else if (!strcmp(argv[1], "appearance-home"))
		test_appearance_leave_retains_scene(fixture, true);
	else if (!strcmp(argv[1], "appearance-close"))
		test_appearance_leave_retains_scene(fixture, false);
	else if (!strcmp(argv[1], "close-appearance-independent"))
		test_close_and_appearance_require_separate_frames(fixture);
	else if (!strcmp(argv[1], "peer-authentication"))
		test_peer_authentication(fixture);
	else if (!strcmp(argv[1], "cancel-cleans-process"))
		test_cancel_cleans_process(fixture);
	else if (!strcmp(argv[1], "key-routing"))
		test_key_routing(fixture);
	else if (!strcmp(argv[1], "input-language"))
		test_input_language(fixture, false);
	else if (!strcmp(argv[1], "input-language-save-fails"))
		test_input_language(fixture, true);
	else if (!strcmp(argv[1], "wake-stays-dark-before-presentation"))
		test_wake_stays_dark_before_presentation(fixture);
	else if (!strcmp(argv[1], "wake-late-commit"))
		test_wake_late_commit(fixture);
	else if (!strcmp(argv[1], "wake-integrated-buffers"))
		test_wake_application_buffers(fixture, false);
	else if (!strcmp(argv[1], "wake-fullscreen-buffer"))
		test_wake_application_buffers(fixture, true);
	else if (!strcmp(argv[1], "wake-output-reset"))
		test_wake_output_reset(fixture, false);
	else if (!strcmp(argv[1], "wake-new-wake"))
		test_wake_output_reset(fixture, true);
	else if (!strcmp(argv[1], "wake-launcher-restart"))
		test_wake_launcher_restart(fixture);
	else if (brightness)
		test_brightness_restore(fixture,
					!strcmp(argv[1], "brightness-crash"));
	else if (keypad_max)
		test_keypad_light(fixture, keypad_max == 1);
	else if (!strcmp(argv[1], "settings-errors-time"))
		test_settings_errors_time(fixture);
	else if (!strcmp(argv[1], "settings-theme-home-before-request"))
		test_settings_choice_home_before_request(fixture, true);
	else if (!strcmp(argv[1], "settings-locale-home-before-request"))
		test_settings_choice_home_before_request(fixture, false);
	else if (!strcmp(argv[1], "battery-notice-frame"))
		test_battery_notice_waits_for_current_frame(fixture, false);
	else if (!strcmp(argv[1], "battery-notice-upgrade"))
		test_battery_notice_waits_for_current_frame(fixture, true);
	else if (!strcmp(argv[1], "battery-charger-wake-charged"))
		test_battery_charger_wake(fixture, "80\n", 80, "");
	else if (!strcmp(argv[1], "battery-charger-wake-low"))
		test_battery_charger_wake(fixture, "10\n", 10, "battery-low");
	else if (!strcmp(argv[1], "battery-charger-wake-critical"))
		test_battery_charger_wake(fixture, "3\n", 3,
					  "battery-critical");
	else if (!strcmp(argv[1], "surface-box") && argc == 7)
		test_surface_box(fixture, atoi(argv[5]), atoi(argv[6]));
	else
		CHECK(false);
	return 0;
}
