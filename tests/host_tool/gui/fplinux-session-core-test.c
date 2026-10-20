/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-core.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fplinux-keypad.h"

#define TEST_PEERS 8
#define TEST_MESSAGES 64
#define TEST_LAUNCHER "org.fplinux.launcher"
#define TEST_APP "org.fplinux.notes"
#define TEST_SETTINGS "org.fplinux.settings"

enum command_type {
	COMMAND_HELLO,
	COMMAND_CONNECT,
	COMMAND_SEND,
	COMMAND_SENT,
	COMMAND_EVENT,
	COMMAND_DISCONNECTED,
	COMMAND_HOLD_CLOSE,
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
	unsigned int teardown_callbacks;
	bool frame_held;
	unsigned int visible_frame;
	unsigned int retained_frame;
	unsigned int captures;
	char root[PATH_MAX];
	char home[PATH_MAX];
	char applications[PATH_MAX];
	char socket_path[PATH_MAX];
	char peer_socket[PATH_MAX];
	char settings[PATH_MAX];
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
	if (sync_fails) {
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
			if (command.message.has_frame) {
				uint16_t first;

				CHECK(command.message.type ==
				      FPLINUX_UI_SESSION_ACTIVE);
				CHECK(pread(command.message.frame_fd, &first,
					    sizeof(first), 0) == sizeof(first));
				CHECK(first == 0xf800);
				close(command.message.frame_fd);
				command.message.frame_fd = -1;
			}
			send_command(channel, &command);
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
	if (enabled)
		fixture->retained_frame = fixture->visible_frame;
}

static int capture_frame(void *context, int *width, int *height)
{
	struct fixture *fixture = context;
	uint16_t first = 0xf800;
	int fd = memfd_create("session-scene", MFD_CLOEXEC);

	CHECK(fd >= 0);
	*width = fixture->width;
	*height = fixture->height;
	CHECK(ftruncate(fd, (off_t)*width * *height * 2) == 0);
	CHECK(pwrite(fd, &first, sizeof(first), 0) == sizeof(first));
	++fixture->captures;
	return fd;
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

static struct fixture *create_fixture(const char *root, int width, int height,
				      bool brightness, unsigned int keypad_max,
				      const char *locale, bool power_supply)
{
	struct fixture *fixture = calloc(1, sizeof(*fixture));
	struct passwd *account = getpwuid(getuid());
	struct fplinux_session_options options;
	char path[PATH_MAX], settings[256];

	CHECK(fixture && account);
	active_fixture = fixture;
	fixture->listener = -1;
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
		       "screen_off_seconds=30\nauto_lock_seconds=off\n"
		       "locale=%s\ninput_locale=en\ntheme=light\nbrightness=7\n",
		       locale) < (int)sizeof(settings));
	write_file(fixture->settings, settings);
	fixture->listener = listen_at(fixture->peer_socket);
	CHECK(fcntl(fixture->listener, F_SETFL, O_NONBLOCK) == 0);
	CHECK(setenv("FPLINUX_TEST_PEER_SOCKET", fixture->peer_socket, 1) == 0);
	CHECK(setenv("TZ", "UTC", 1) == 0);
	tzset();
	add_entry(fixture, TEST_LAUNCHER, "launcher");
	add_entry(fixture, TEST_APP, "integrated");
	add_entry(fixture, TEST_SETTINGS, "integrated");
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
	if (power_supply) {
		char supply[PATH_MAX];

		path_join(fixture->power_supply, root, "power-supply");
		CHECK(mkdir(fixture->power_supply, 0700) == 0);
		path_join(supply, fixture->power_supply, "battery");
		CHECK(mkdir(supply, 0700) == 0);
		path_join(path, supply, "type");
		write_file(path, "Battery\n");
		path_join(fixture->battery_capacity, supply, "capacity");
		write_file(fixture->battery_capacity, "3\n");
		path_join(supply, fixture->power_supply, "charger");
		CHECK(mkdir(supply, 0700) == 0);
		path_join(path, supply, "type");
		write_file(path, "Unknown\n");
		path_join(fixture->charger_online, supply, "online");
		write_file(fixture->charger_online, "0\n");
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
		.languages_dir = fixture->languages,
		.brightness_socket = brightness ? fixture->brightness_socket :
						  NULL,
		.leds_dir = keypad_max ? fixture->leds : NULL,
		.power_supply_dir = power_supply ? fixture->power_supply : NULL,
		.clock_ms = clock_ms,
		.changed = session_changed,
		.hold_frame = hold_frame,
		.capture_frame = capture_frame,
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
		for (size_t i = 0; i < fixture->peer_count; ++i) {
			struct peer *peer = &fixture->peers[i];
			struct command event;
			ssize_t bytes;

			while ((bytes = recv(peer->fd, &event, sizeof(event),
					     0)) > 0) {
				CHECK(bytes == (ssize_t)sizeof(event));
				if (event.type == COMMAND_SENT)
					++peer->sent;
				else if (event.type == COMMAND_DISCONNECTED)
					peer->disconnected = true;
				else {
					CHECK(event.type == COMMAND_EVENT);
					CHECK(peer->message_count <
					      TEST_MESSAGES);
					peer->messages[peer->message_count++] =
						event.message;
				}
			}
		}
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
	while (peer->sent == sent && real_ms() < deadline)
		pump(fixture, 2);
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

static struct fplinux_ui_session_message
open_end_prompt(struct fixture *fixture, struct peer *launcher,
		struct peer *app)
{
	struct fplinux_ui_session_message ignored;
	char saved[PATH_MAX];
	unsigned int captures = fixture->captures;

	clear_messages(fixture, launcher);
	clear_messages(fixture, app);
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_POWER));
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, app->id);

	CHECK(active.end_prompt && active.token);
	CHECK(active.has_frame && active.width == fixture->width &&
	      active.height == fixture->height &&
	      fixture->captures == captures + 1);
	CHECK(active.token ==
	      fplinux_session_find_process(fixture->session, app->pid)->token);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	path_join(saved, fixture->home, app->id);
	CHECK(access(saved, F_OK) == -1 && errno == ENOENT);
	return active;
}

static void show_end_prompt(struct fixture *fixture, struct peer *launcher,
			    struct peer *app,
			    const struct fplinux_ui_session_message *active)
{
	acknowledge(fixture, launcher, active);
	CHECK(fplinux_session_launcher_on_top(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	fplinux_session_set_launcher_shown(fixture->session, true);
	pump(fixture, 10);
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
	struct fplinux_ui_session_message visibility =
		wait_message(fixture, app, FPLINUX_UI_SESSION_VISIBILITY, NULL);

	CHECK(!visibility.visible);
	CHECK(fplinux_session_find_process(fixture->session, app->pid)
		      ->lifecycle == FPLINUX_UI_SESSION_FOREGROUND);
}

static void end_action(struct fixture *fixture, struct peer *launcher,
		       const struct fplinux_ui_session_message *active,
		       const char *action)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_END,
		.token = active->token,
	};

	strcpy(message.app_id, active->active_id);
	strcpy(message.value, action);
	issue(fixture, launcher, &message);
}

static void test_end_prompt_minimize_retains(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message active =
		open_end_prompt(fixture, launcher, app);
	pid_t pid = app->pid;

	show_end_prompt(fixture, launcher, app, &active);
	clear_messages(fixture, launcher);
	end_action(fixture, launcher, &active, "minimize");
	wait_message(fixture, launcher, FPLINUX_UI_SESSION_HOME, NULL);
	active = active_for(fixture, launcher, "");
	CHECK(!active.end_prompt && !active.token);
	CHECK(!strcmp(active.value, "home"));
	CHECK(fixture->frame_held);
	acknowledge(fixture, launcher, &active);
	CHECK(!fixture->frame_held);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, pid);
	struct fplinux_ui_session_message ignored;

	CHECK(process && !process->closing && !process->shown);
	CHECK(process->lifecycle == FPLINUX_UI_SESSION_BACKGROUND);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	fplinux_session_set_launcher_shown(fixture->session, false);
	CHECK(launch_app(fixture, launcher, TEST_APP)->pid == pid);
	active = active_for(fixture, launcher, TEST_APP);
	acknowledge(fixture, launcher, &active);
	CHECK(!active.end_prompt);
	CHECK(fplinux_session_focused_pid(fixture->session) == pid);
	CHECK(fplinux_session_find_process(fixture->session, pid)->shown);
}

static void test_end_prompt_close_saves(struct fixture *fixture, bool red)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message active =
		open_end_prompt(fixture, launcher, app);
	char saved[PATH_MAX];

	if (!red)
		show_end_prompt(fixture, launcher, app, &active);
	clear_messages(fixture, launcher);
	if (red)
		CHECK(!phone_gesture(fixture, FPLINUX_KEY_POWER));
	else
		end_action(fixture, launcher, &active, "close");
	wait_message(fixture, launcher, FPLINUX_UI_SESSION_HOME, NULL);
	active = active_for(fixture, launcher, "");
	CHECK(!active.end_prompt && !active.token);
	CHECK(!strcmp(active.value, "home"));
	CHECK(fixture->frame_held);
	acknowledge(fixture, launcher, &active);
	CHECK(!fixture->frame_held);
	wait_message(fixture, app, FPLINUX_UI_SESSION_CLOSE, NULL);
	wait_reaped(fixture, app->pid);
	path_join(saved, fixture->home, TEST_APP);
	expect_file(saved, "saved\n");
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
}

static void test_end_prompt_cancel_restores(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message active =
		open_end_prompt(fixture, launcher, app);
	uint32_t token = active.token;
	struct fplinux_ui_session_message ignored;

	show_end_prompt(fixture, launcher, app, &active);
	clear_messages(fixture, launcher);
	clear_messages(fixture, app);
	end_action(fixture, launcher, &active, "cancel");
	struct fplinux_ui_session_message restored =
		active_for(fixture, launcher, TEST_APP);

	CHECK(!restored.end_prompt && restored.token == token);
	CHECK(!strcmp(restored.value, active.value));
	CHECK(!strcmp(restored.name, active.name));
	CHECK(!strcmp(restored.selected_id, active.selected_id));
	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	CHECK(!fixture->frame_held);
	struct fplinux_ui_session_message visibility =
		wait_message(fixture, app, FPLINUX_UI_SESSION_VISIBILITY, NULL);

	CHECK(visibility.visible);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	CHECK(!take_message(launcher, FPLINUX_UI_SESSION_HOME, NULL, &ignored));
	fplinux_session_set_launcher_shown(fixture->session, false);
	active = open_end_prompt(fixture, launcher, app);
	CHECK(!fplinux_session_key(fixture->session, FPLINUX_KEY_POWER, true,
				   fixture->now_ms));
	CHECK(!fplinux_session_key(fixture->session, FPLINUX_KEY_POWER, false,
				   fixture->now_ms + 5000));
	pump(fixture, 10);
	CHECK(fplinux_session_find_process(fixture->session, app->pid));
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	end_action(fixture, launcher, &active, "cancel");
}

static void test_end_prompt_stale_instance(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message old =
		open_end_prompt(fixture, launcher, app);
	struct command crash = { .type = COMMAND_CRASH };
	pid_t old_pid = app->pid;

	send_command(app->fd, &crash);
	wait_reaped(fixture, old_pid);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, "");

	CHECK(!active.end_prompt);
	clear_messages(fixture, launcher);
	app = show_app(fixture, launcher, TEST_APP);
	CHECK(app->pid != old_pid);
	active = open_end_prompt(fixture, launcher, app);
	CHECK(active.token != old.token);
	end_action(fixture, launcher, &old, "close");
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, app->pid);
	struct fplinux_ui_session_message ignored;

	CHECK(process && !process->closing && process->shown);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_POWER));
	active = active_for(fixture, launcher, "");
	acknowledge(fixture, launcher, &active);
	wait_message(fixture, app, FPLINUX_UI_SESSION_CLOSE, NULL);
	wait_reaped(fixture, app->pid);
}

static void test_end_prompt_lock_cancels(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_SETTINGS);
	struct fplinux_ui_session_message setting = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.name = "auto_lock_seconds",
		.value = "0",
	};

	issue(fixture, app, &setting);
	wait_message(fixture, app, FPLINUX_UI_SESSION_SETTING,
		     "auto_lock_seconds");
	struct fplinux_ui_session_message active =
		open_end_prompt(fixture, launcher, app);

	show_end_prompt(fixture, launcher, app, &active);
	clear_messages(fixture, launcher);
	fixture->now_ms += 30000;
	pump(fixture, 10);
	CHECK(fplinux_session_is_locked(fixture->session));
	struct fplinux_ui_session_message locked =
		active_for(fixture, launcher, TEST_SETTINGS);

	CHECK(locked.locked && !locked.end_prompt);
	end_action(fixture, launcher, &active, "close");
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, app->pid);
	struct fplinux_ui_session_message ignored;

	CHECK(process && !process->closing);
	CHECK(!take_message(app, FPLINUX_UI_SESSION_CLOSE, NULL, &ignored));
}

static void test_end_prompt_update_keeps_focus(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);
	struct peer *app = show_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message active =
		open_end_prompt(fixture, launcher, app);
	struct fplinux_ui_session_message title = {
		.type = FPLINUX_UI_SESSION_TITLE,
		.title = "Current draft",
	};

	show_end_prompt(fixture, launcher, app, &active);
	clear_messages(fixture, launcher);
	issue(fixture, app, &title);
	struct fplinux_ui_session_message update =
		active_for(fixture, launcher, TEST_APP);

	CHECK(update.end_prompt && update.serial != active.serial);
	CHECK(!strcmp(update.title, "Current draft"));
	/* The update has no ACK yet; the already drawn prompt keeps the keys. */
	CHECK(fplinux_session_launcher_on_top(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == launcher->pid);
	end_action(fixture, launcher, &active, "cancel");
}

static void
test_end_prompt_fullscreen_restarts_launcher(struct fixture *fixture)
{
	struct peer *launcher = start_launcher(fixture);

	add_entry(fixture, TEST_APP, "fullscreen");
	struct peer *app = launch_app(fixture, launcher, TEST_APP);
	struct fplinux_ui_session_message config =
		authenticate(fixture, app, NULL);

	commit_app(fixture, app, &config);
	wait_reaped(fixture, launcher->pid);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	clear_messages(fixture, app);
	CHECK(!phone_gesture(fixture, FPLINUX_KEY_POWER));
	launcher = wait_peer(fixture, TEST_LAUNCHER);
	authenticate(fixture, launcher, NULL);
	struct fplinux_ui_session_message active =
		active_for(fixture, launcher, TEST_APP);

	CHECK(active.end_prompt && active.token);
	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	send_type(fixture, launcher, FPLINUX_UI_SESSION_READY);
	fplinux_session_surface_committed(fixture->session, launcher->pid,
					  fixture->width, fixture->height);
	fplinux_session_frame(fixture->session, launcher->pid);
	CHECK(!fplinux_session_launcher_on_top(fixture->session));
	show_end_prompt(fixture, launcher, app, &active);
	end_action(fixture, launcher, &active, "cancel");
	wait_reaped(fixture, launcher->pid);
	CHECK(fplinux_session_focused_pid(fixture->session) == app->pid);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(fixture->session, app->pid);

	CHECK(process && process->shown && !process->closing);
	CHECK(process->mode == FPLINUX_UI_SESSION_FULLSCREEN);
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

static void test_appearance_end_keeps_visible_dialog(struct fixture *fixture,
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
	active = open_end_prompt(fixture, launcher, settings);
	show_end_prompt(fixture, launcher, settings, &active);
	/* The controlled compositor now shows the END dialog above the old scene. */
	fixture->visible_frame = 2;
	end_action(fixture, launcher, &active, retain ? "minimize" : "close");
	active = active_for(fixture, launcher, "");
	CHECK(fixture->frame_held && fixture->retained_frame == 2);
	acknowledge(fixture, launcher, &active);
	CHECK(!fixture->frame_held);
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

int main(int argc, char **argv)
{
	if (getenv("FPLINUX_APP_ID"))
		return run_peer();
	CHECK(argc == 3 || argc == 7);
	CHECK(atexit(cleanup) == 0);
	bool brightness = !strcmp(argv[1], "brightness-home") ||
			  !strcmp(argv[1], "brightness-crash") ||
			  !strncmp(argv[1], "task-view-", 10) ||
			  !strncmp(argv[1], "wake-", 5);
	unsigned int keypad_max = 0;
	int width = argc == 7 ? atoi(argv[3]) : 240;
	int height = argc == 7 ? atoi(argv[4]) : 320;
	struct fixture *fixture;

	if (!strcmp(argv[1], "keypad-light"))
		keypad_max = 255;
	else if (!strcmp(argv[1], "keypad-light-binary"))
		keypad_max = 1;
	fixture = create_fixture(
		argv[2], width, height, brightness, keypad_max,
		!strcmp(argv[1], "task-localized-title") ? "ru" : "en",
		!strncmp(argv[1], "battery-", 8));

	if (!strcmp(argv[1], "startup-frame-before-ready"))
		test_startup_frame_before_ready(fixture);
	else if (!strcmp(argv[1], "home-retains-exit-saves"))
		test_home_retains_exit_saves(fixture);
	else if (!strcmp(argv[1], "shutdown-saves-bounded"))
		test_shutdown_saves_and_bounds_unresponsive_peers(fixture);
	else if (!strcmp(argv[1], "task-controls"))
		test_task_controls(fixture);
	else if (!strcmp(argv[1], "task-localized-title"))
		test_task_localized_title(fixture);
	else if (!strcmp(argv[1], "task-stale-lock"))
		test_task_stale_and_locked(fixture);
	else if (!strcmp(argv[1], "end-prompt-minimize"))
		test_end_prompt_minimize_retains(fixture);
	else if (!strcmp(argv[1], "end-prompt-close"))
		test_end_prompt_close_saves(fixture, false);
	else if (!strcmp(argv[1], "end-prompt-red-close"))
		test_end_prompt_close_saves(fixture, true);
	else if (!strcmp(argv[1], "end-prompt-cancel"))
		test_end_prompt_cancel_restores(fixture);
	else if (!strcmp(argv[1], "end-prompt-stale-instance"))
		test_end_prompt_stale_instance(fixture);
	else if (!strcmp(argv[1], "end-prompt-lock"))
		test_end_prompt_lock_cancels(fixture);
	else if (!strcmp(argv[1], "end-prompt-updates"))
		test_end_prompt_update_keeps_focus(fixture);
	else if (!strcmp(argv[1], "end-prompt-fullscreen"))
		test_end_prompt_fullscreen_restarts_launcher(fixture);
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
	else if (!strcmp(argv[1], "appearance-end-minimize"))
		test_appearance_end_keeps_visible_dialog(fixture, true);
	else if (!strcmp(argv[1], "appearance-end-close"))
		test_appearance_end_keeps_visible_dialog(fixture, false);
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
