// SPDX-License-Identifier: GPL-2.0-only
#include "fplinux-brightness-client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int exchange(struct fplinux_brightness_client *client,
		    const char *request, unsigned int *level)
{
	char response[32];
	unsigned int value;
	int error;
	int parsed_length = 0;
	ssize_t length;

	if (!client || client->fd < 0) {
		errno = ENOTCONN;
		return -1;
	}
	for (;;) {
		length = send(client->fd, request, strlen(request),
			      MSG_NOSIGNAL);
		if (length < 0 && errno == EINTR)
			continue;
		break;
	}
	if (length != (ssize_t)strlen(request)) {
		if (length >= 0)
			errno = EIO;
		return -1;
	}
	for (;;) {
		length = recv(client->fd, response, sizeof(response) - 1, 0);
		if (length < 0 && errno == EINTR)
			continue;
		break;
	}
	if (length <= 0) {
		if (!length)
			errno = ECONNRESET;
		return -1;
	}
	response[length] = '\0';
	if (!strcmp(response, "OK") && !level)
		return 0;
	if (level &&
	    sscanf(response, "LEVEL %u%n", &value, &parsed_length) == 1 &&
	    parsed_length == length && value <= 10) {
		*level = value;
		return 0;
	}
	parsed_length = 0;
	if (sscanf(response, "ERR %d%n", &error, &parsed_length) == 1 &&
	    parsed_length == length && error > 0) {
		errno = error;
		return -1;
	}
	errno = EPROTO;
	return -1;
}

int fplinux_brightness_connect(struct fplinux_brightness_client *client,
			       const char *socket_path)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	const char *path = socket_path ? socket_path :
					 FPLINUX_BRIGHTNESS_SOCKET;
	int fd;

	if (!client || strlen(path) >= sizeof(address.sun_path)) {
		errno = EINVAL;
		return -1;
	}
	client->fd = -1;
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	strcpy(address.sun_path, path);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int error = errno;

		close(fd);
		errno = error;
		return -1;
	}
	client->fd = fd;
	return 0;
}

int fplinux_brightness_get(struct fplinux_brightness_client *client,
			   unsigned int *level)
{
	if (!level) {
		errno = EINVAL;
		return -1;
	}
	return exchange(client, "GET", level);
}

static int send_level(struct fplinux_brightness_client *client,
		      const char *verb, unsigned int level)
{
	char request[16];

	if (level > 10) {
		errno = EINVAL;
		return -1;
	}
	snprintf(request, sizeof(request), "%s %u", verb, level);
	return exchange(client, request, NULL);
}

int fplinux_brightness_set(struct fplinux_brightness_client *client,
			   unsigned int level)
{
	return send_level(client, "SET", level);
}

int fplinux_brightness_claim(struct fplinux_brightness_client *client)
{
	return exchange(client, "CLAIM", NULL);
}

int fplinux_brightness_show(struct fplinux_brightness_client *client,
			    unsigned int level)
{
	return send_level(client, "SHOW", level);
}

int fplinux_brightness_release(struct fplinux_brightness_client *client)
{
	return exchange(client, "RELEASE", NULL);
}

void fplinux_brightness_close(struct fplinux_brightness_client *client)
{
	if (client && client->fd >= 0) {
		close(client->fd);
		client->fd = -1;
	}
}
