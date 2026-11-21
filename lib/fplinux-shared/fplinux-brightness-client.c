// SPDX-License-Identifier: GPL-2.0-only
#include "fplinux-brightness-client.h"
#include "fplinux-brightness-wire.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int exchange(struct fplinux_brightness_client *client,
		    enum fplinux_brightness_type type, unsigned int value,
		    unsigned int *level)
{
	const struct fplinux_brightness_message request = {
		.type = type,
		.level = value,
	};
	struct fplinux_brightness_message response;
	int result;

	if (!client || client->fd < 0) {
		errno = ENOTCONN;
		return -1;
	}
	if (fplinux_brightness_wire_send(client->fd, &request) < 0)
		return -1;
	do {
		result = fplinux_brightness_wire_receive(
			client->fd, FPLINUX_BRIGHTNESS_REPLY, &response);
	} while (result < 0 && errno == EINTR);
	if (result <= 0) {
		if (!result)
			errno = ECONNRESET;
		return -1;
	}
	if (response.type == FPLINUX_BRIGHTNESS_OK && !level)
		return 0;
	if (response.type == FPLINUX_BRIGHTNESS_LEVEL && level) {
		*level = response.level;
		return 0;
	}
	errno = response.type == FPLINUX_BRIGHTNESS_ERROR ? response.error :
							    EPROTO;
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
	return exchange(client, FPLINUX_BRIGHTNESS_GET, 0, level);
}

static int send_level(struct fplinux_brightness_client *client,
		      enum fplinux_brightness_type type, unsigned int level)
{
	if (level > FPLINUX_BRIGHTNESS_LEVEL_MAX) {
		errno = EINVAL;
		return -1;
	}
	return exchange(client, type, level, NULL);
}

int fplinux_brightness_set(struct fplinux_brightness_client *client,
			   unsigned int level)
{
	return send_level(client, FPLINUX_BRIGHTNESS_SET, level);
}

int fplinux_brightness_claim(struct fplinux_brightness_client *client)
{
	return exchange(client, FPLINUX_BRIGHTNESS_CLAIM, 0, NULL);
}

int fplinux_brightness_show(struct fplinux_brightness_client *client,
			    unsigned int level)
{
	return send_level(client, FPLINUX_BRIGHTNESS_SHOW, level);
}

int fplinux_brightness_release(struct fplinux_brightness_client *client)
{
	return exchange(client, FPLINUX_BRIGHTNESS_RELEASE, 0, NULL);
}

void fplinux_brightness_close(struct fplinux_brightness_client *client)
{
	if (client && client->fd >= 0) {
		close(client->fd);
		client->fd = -1;
	}
}
