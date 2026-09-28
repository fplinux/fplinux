// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "fplinux-brightness-client.h"
#include "fplinux-cli.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CLIENT_COUNT 32
#define PATH_CAPACITY 256

enum { OPTION_CONFIG, OPTION_BACKLIGHT, OPTION_SOCKET, OPTION_STATE };

struct brightness_service {
	char backlight[PATH_CAPACITY];
	const char *socket_path;
	const char *state_path;
	unsigned int levels[11];
	unsigned int baseline;
	int listener;
	int lock;
	int lease;
	int clients[CLIENT_COUNT];
};

static volatile sig_atomic_t stopped;

static void stop_signal(int signum)
{
	(void)signum;
	stopped = 1;
}

static bool decimal(const char *text, unsigned int maximum, unsigned int *value)
{
	unsigned int result = 0;
	const char *cursor = text;

	if (!*cursor || (*cursor == '0' && cursor[1]))
		return false;
	for (; *cursor; ++cursor) {
		unsigned int digit;

		if (*cursor < '0' || *cursor > '9')
			return false;
		digit = (unsigned int)(*cursor - '0');
		if (result > (maximum - digit) / 10)
			return false;
		result = result * 10 + digit;
	}
	*value = result;
	return true;
}

static bool backlight_name_valid(const char *name)
{
	const char *cursor;

	if (!*name || !strcmp(name, ".") || !strcmp(name, ".."))
		return false;
	for (cursor = name; *cursor; ++cursor) {
		if ((*cursor >= 'a' && *cursor <= 'z') ||
		    (*cursor >= 'A' && *cursor <= 'Z') ||
		    (*cursor >= '0' && *cursor <= '9') || *cursor == '_' ||
		    *cursor == '-' || *cursor == '.')
			continue;
		return false;
	}
	return true;
}

static int load_config(struct brightness_service *service, const char *path,
		       const char *backlight_override)
{
	char data[256];
	char *name, *levels, *cursor, *end;
	FILE *file;
	size_t length;
	unsigned int previous = 0;
	int i;

	file = fopen(path, "r");
	if (!file)
		return -1;
	length = fread(data, 1, sizeof(data), file);
	if (ferror(file) || length == sizeof(data)) {
		fclose(file);
		errno = EINVAL;
		return -1;
	}
	fclose(file);
	data[length] = '\0';
	if (memchr(data, '\0', length) || strncmp(data, "backlight=", 10))
		goto invalid;
	name = data + 10;
	end = strchr(name, '\n');
	if (!end)
		goto invalid;
	*end++ = '\0';
	if (!backlight_name_valid(name) || strncmp(end, "levels=", 7))
		goto invalid;
	levels = end + 7;
	end = strchr(levels, '\n');
	if (!end || end[1])
		goto invalid;
	*end = '\0';
	cursor = levels;
	for (i = 0; i < 11; ++i) {
		unsigned int raw;

		end = strchr(cursor, ',');
		if ((i < 10 && !end) || (i == 10 && end))
			goto invalid;
		if (end)
			*end = '\0';
		if (!decimal(cursor, 63, &raw) || (i == 0 && raw != 0) ||
		    (i > 0 && raw <= previous))
			goto invalid;
		service->levels[i] = raw;
		previous = raw;
		cursor = end ? end + 1 : NULL;
	}
	if (snprintf(service->backlight, sizeof(service->backlight), "%s",
		     backlight_override ? backlight_override : name) >=
	    (int)sizeof(service->backlight))
		goto invalid;
	if (!backlight_override &&
	    snprintf(service->backlight, sizeof(service->backlight),
		     "/sys/class/backlight/%s",
		     name) >= (int)sizeof(service->backlight))
		goto invalid;
	return 0;

invalid:
	errno = EINVAL;
	return -1;
}

static int attribute_path(char *path, size_t capacity,
			  const struct brightness_service *service,
			  const char *attribute)
{
	if (snprintf(path, capacity, "%s/%s", service->backlight, attribute) >=
	    (int)capacity) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int check_maximum(const struct brightness_service *service)
{
	char path[PATH_CAPACITY + 32], data[32];
	unsigned int maximum;
	FILE *file;
	size_t length;

	if (attribute_path(path, sizeof(path), service, "max_brightness") < 0)
		return -1;
	file = fopen(path, "r");
	if (!file)
		return -1;
	length = fread(data, 1, sizeof(data) - 1, file);
	if (ferror(file) || length == sizeof(data) - 1) {
		fclose(file);
		errno = EINVAL;
		return -1;
	}
	fclose(file);
	data[length] = '\0';
	if (length && data[length - 1] == '\n')
		data[--length] = '\0';
	if (!decimal(data, 65535, &maximum) || maximum < service->levels[10]) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int apply_level(const struct brightness_service *service,
		       unsigned int level)
{
	char path[PATH_CAPACITY + 32], data[8];
	int fd, length, error;

	if (attribute_path(path, sizeof(path), service, "brightness") < 0)
		return -1;
	fd = open(path, O_WRONLY | O_CLOEXEC | O_TRUNC);
	if (fd < 0)
		return -1;
	length = snprintf(data, sizeof(data), "%u\n", service->levels[level]);
	if (write(fd, data, length) != length) {
		error = errno ? errno : EIO;
		close(fd);
		errno = error;
		return -1;
	}
	if (close(fd) < 0)
		return -1;
	return 0;
}

static unsigned int load_state(const char *path)
{
	char data[8];
	unsigned int level;
	FILE *file = fopen(path, "r");
	size_t length;

	if (!file)
		return 7;
	length = fread(data, 1, sizeof(data) - 1, file);
	if (ferror(file) || length == sizeof(data) - 1) {
		fclose(file);
		return 7;
	}
	fclose(file);
	data[length] = '\0';
	if (!length || data[length - 1] != '\n')
		return 7;
	data[length - 1] = '\0';
	return decimal(data, 10, &level) ? level : 7;
}

static int publish_state(const char *path, unsigned int level)
{
	char temporary[PATH_CAPACITY + 32], data[8];
	int fd, length, error;

	if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >=
	    (int)sizeof(temporary)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = mkstemp(temporary);
	if (fd < 0)
		return -1;
	if (fchmod(fd, 0600) < 0)
		goto failed;
	length = snprintf(data, sizeof(data), "%u\n", level);
	if (write(fd, data, length) != length) {
		if (!errno)
			errno = EIO;
		goto failed;
	}
	if (close(fd) < 0) {
		unlink(temporary);
		return -1;
	}
	if (rename(temporary, path) < 0) {
		unlink(temporary);
		return -1;
	}
	return 0;

failed:
	error = errno;
	close(fd);
	unlink(temporary);
	errno = error;
	return -1;
}

static int make_listener(struct brightness_service *service)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct stat current;
	char lock_path[PATH_CAPACITY + 16];
	int fd;

	if (strlen(service->socket_path) >= sizeof(address.sun_path) ||
	    snprintf(lock_path, sizeof(lock_path), "%s.lock",
		     service->socket_path) >= (int)sizeof(lock_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	service->lock = open(lock_path,
			     O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (service->lock < 0 || flock(service->lock, LOCK_EX | LOCK_NB) < 0)
		return -1;
	if (fchmod(service->lock, 0600) < 0)
		return -1;
	if (lstat(service->socket_path, &current) == 0) {
		if (!S_ISSOCK(current.st_mode)) {
			errno = EEXIST;
			return -1;
		}
		if (unlink(service->socket_path) < 0)
			return -1;
	} else if (errno != ENOENT) {
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	strcpy(address.sun_path, service->socket_path);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    chmod(service->socket_path, 0600) < 0 ||
	    listen(fd, CLIENT_COUNT) < 0) {
		int error = errno;

		close(fd);
		unlink(service->socket_path);
		errno = error;
		return -1;
	}
	service->listener = fd;
	return 0;
}

static int reply(int fd, int error, int level)
{
	char response[32];
	int length;

	if (error)
		length = snprintf(response, sizeof(response), "ERR %d", error);
	else if (level >= 0)
		length =
			snprintf(response, sizeof(response), "LEVEL %d", level);
	else
		length = snprintf(response, sizeof(response), "OK");
	return send(fd, response, length, MSG_NOSIGNAL) == length ? 0 : -1;
}

static bool parse_level(const char *request, const char *verb,
			unsigned int *level)
{
	return !strncmp(request, verb, strlen(verb)) &&
	       request[strlen(verb)] == ' ' &&
	       decimal(request + strlen(verb) + 1, 10, level);
}

static void close_client(struct brightness_service *service, int index)
{
	int fd = service->clients[index];

	if (service->lease == fd) {
		service->lease = -1;
		if (apply_level(service, service->baseline) < 0)
			fprintf(stderr, "fplinux-brightnessd: restore: %s\n",
				strerror(errno));
	}
	close(fd);
	service->clients[index] = -1;
}

static void handle_client(struct brightness_service *service, int index)
{
	char request[32];
	unsigned int level;
	int fd = service->clients[index];
	ssize_t length;
	int error = 0;

	length = recv(fd, request, sizeof(request) - 1,
		      MSG_DONTWAIT | MSG_TRUNC);
	if (length < 0) {
		if (errno != EINTR && errno != EAGAIN)
			close_client(service, index);
		return;
	}
	if (!length) {
		close_client(service, index);
		return;
	}
	if (length >= (ssize_t)sizeof(request)) {
		reply(fd, EINVAL, -1);
		return;
	}
	if (memchr(request, '\0', length)) {
		reply(fd, EINVAL, -1);
		return;
	}
	request[length] = '\0';
	if (!strcmp(request, "GET")) {
		reply(fd, 0, service->baseline);
		return;
	}
	if (parse_level(request, "SET", &level)) {
		if (publish_state(service->state_path, level) < 0)
			error = errno;
		else {
			service->baseline = level;
			if (service->lease < 0 &&
			    apply_level(service, level) < 0)
				error = errno;
		}
	} else if (!strcmp(request, "CLAIM")) {
		if (service->lease >= 0)
			error = EBUSY;
		else
			service->lease = fd;
	} else if (parse_level(request, "SHOW", &level)) {
		if (service->lease != fd)
			error = EPERM;
		else if (apply_level(service, level) < 0)
			error = errno;
	} else if (!strcmp(request, "RELEASE")) {
		if (service->lease != fd)
			error = EPERM;
		else if (apply_level(service, service->baseline) < 0)
			error = errno;
		else
			service->lease = -1;
	} else {
		error = EINVAL;
	}
	if (reply(fd, error, -1) < 0)
		close_client(service, index);
}

static int serve(struct brightness_service *service)
{
	struct pollfd pollfds[CLIENT_COUNT + 1];
	int indexes[CLIENT_COUNT + 1];
	int i, count, result;

	while (!stopped) {
		count = 1;
		pollfds[0] = (struct pollfd){ .fd = service->listener,
					      .events = POLLIN };
		for (i = 0; i < CLIENT_COUNT; ++i) {
			if (service->clients[i] < 0)
				continue;
			indexes[count] = i;
			pollfds[count++] =
				(struct pollfd){ .fd = service->clients[i],
						 .events = POLLIN };
		}
		result = poll(pollfds, count, -1);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		for (i = 1; i < count; ++i) {
			if (pollfds[i].revents & POLLIN)
				handle_client(service, indexes[i]);
			if (service->clients[indexes[i]] >= 0 &&
			    (pollfds[i].revents &
			     (POLLERR | POLLHUP | POLLNVAL)))
				close_client(service, indexes[i]);
		}
		if (pollfds[0].revents & POLLIN) {
			int fd = accept4(service->listener, NULL, NULL,
					 SOCK_CLOEXEC | SOCK_NONBLOCK);

			if (fd < 0)
				continue;
			for (i = 0; i < CLIENT_COUNT; ++i)
				if (service->clients[i] < 0)
					break;
			if (i < CLIENT_COUNT)
				service->clients[i] = fd;
			else
				close(fd);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct fplinux_cli_option options[] = {
		[OPTION_CONFIG] = { .name = "config",
				    .metavar = "FILE",
				    .help = "Read a target brightness table." },
		[OPTION_BACKLIGHT] = { .name = "backlight",
				       .metavar = "DIR",
				       .help = "Use a selected backlight directory." },
		[OPTION_SOCKET] = { .name = "socket",
				    .metavar = "PATH",
				    .help = "Listen on a selected socket." },
		[OPTION_STATE] = { .name = "state",
				   .metavar = "FILE",
				   .help = "Store the desired logical level." },
	};
	struct fplinux_cli cli = {
		.program = "fplinux-brightnessd",
		.description = "Own the display brightness and preview lease.",
		.options = options,
		.option_count = sizeof(options) / sizeof(options[0]),
	};
	struct brightness_service service = {
		.socket_path = FPLINUX_BRIGHTNESS_SOCKET,
		.state_path = "/run/fplinux-brightness.state",
		.listener = -1,
		.lock = -1,
		.lease = -1,
	};
	const char *config = "/etc/fplinux/brightness.conf";
	enum fplinux_cli_result parsed;
	int i, result = 1;

	parsed = fplinux_cli_parse(&cli, argc, argv);
	if (parsed != FPLINUX_CLI_READY)
		return parsed;
	if (options[OPTION_CONFIG].count)
		config = options[OPTION_CONFIG].value;
	if (options[OPTION_SOCKET].count)
		service.socket_path = options[OPTION_SOCKET].value;
	if (options[OPTION_STATE].count)
		service.state_path = options[OPTION_STATE].value;
	for (i = 0; i < CLIENT_COUNT; ++i)
		service.clients[i] = -1;
	umask(077);
	if (load_config(&service, config, options[OPTION_BACKLIGHT].value) <
		    0 ||
	    check_maximum(&service) < 0) {
		fprintf(stderr,
			"fplinux-brightnessd: invalid or unavailable backlight: %s\n",
			strerror(errno));
		return 1;
	}
	service.baseline = load_state(service.state_path);
	if (make_listener(&service) < 0 ||
	    apply_level(&service, service.baseline) < 0 ||
	    publish_state(service.state_path, service.baseline) < 0) {
		fprintf(stderr, "fplinux-brightnessd: startup: %s\n",
			strerror(errno));
		goto done;
	}
	signal(SIGTERM, stop_signal);
	signal(SIGINT, stop_signal);
	result = serve(&service);
	if (result < 0)
		fprintf(stderr, "fplinux-brightnessd: poll: %s\n",
			strerror(errno));

done:
	for (i = 0; i < CLIENT_COUNT; ++i)
		if (service.clients[i] >= 0)
			close_client(&service, i);
	if (service.listener >= 0) {
		close(service.listener);
		unlink(service.socket_path);
	}
	if (service.lock >= 0)
		close(service.lock);
	return result < 0 ? 1 : result;
}
