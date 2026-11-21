// SPDX-License-Identifier: GPL-2.0-only
#define _DEFAULT_SOURCE
#include "fplinux-brightness-wire.h"

#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

static const uint8_t packet_sizes[] = {
	[FPLINUX_BRIGHTNESS_GET] = 1,	  [FPLINUX_BRIGHTNESS_SET] = 2,
	[FPLINUX_BRIGHTNESS_CLAIM] = 1,	  [FPLINUX_BRIGHTNESS_SHOW] = 2,
	[FPLINUX_BRIGHTNESS_RELEASE] = 1, [FPLINUX_BRIGHTNESS_LEVEL] = 2,
	[FPLINUX_BRIGHTNESS_OK] = 1,	  [FPLINUX_BRIGHTNESS_ERROR] = 5,
};

int fplinux_brightness_wire_send(
	int fd, const struct fplinux_brightness_message *message)
{
	uint8_t packet[5];
	size_t size;
	ssize_t bytes;

	packet[0] = message->type;
	switch (message->type) {
	case FPLINUX_BRIGHTNESS_SET:
	case FPLINUX_BRIGHTNESS_SHOW:
	case FPLINUX_BRIGHTNESS_LEVEL:
		if (message->level > FPLINUX_BRIGHTNESS_LEVEL_MAX)
			goto invalid;
		packet[1] = message->level;
		break;
	case FPLINUX_BRIGHTNESS_ERROR: {
		uint32_t error;

		if (message->error <= 0)
			goto invalid;
		error = htole32(message->error);
		memcpy(packet + 1, &error, sizeof(error));
		break;
	}
	case FPLINUX_BRIGHTNESS_GET:
	case FPLINUX_BRIGHTNESS_CLAIM:
	case FPLINUX_BRIGHTNESS_RELEASE:
	case FPLINUX_BRIGHTNESS_OK:
		break;
	default:
		goto invalid;
	}
	size = packet_sizes[message->type];
	do {
		bytes = send(fd, packet, size, MSG_NOSIGNAL);
	} while (bytes < 0 && errno == EINTR);
	if (bytes == (ssize_t)size)
		return 0;
	if (bytes >= 0)
		errno = EIO;
	return -1;

invalid:
	errno = EINVAL;
	return -1;
}

int fplinux_brightness_wire_receive(int fd,
				    enum fplinux_brightness_direction direction,
				    struct fplinux_brightness_message *message)
{
	uint8_t packet[5];
	ssize_t bytes = recv(fd, packet, sizeof(packet), MSG_TRUNC);
	unsigned int type;

	if (bytes <= 0)
		return (int)bytes;
	type = packet[0];
	if (type >= sizeof(packet_sizes) || bytes != packet_sizes[type] ||
	    (direction == FPLINUX_BRIGHTNESS_REQUEST) !=
		    (type <= FPLINUX_BRIGHTNESS_RELEASE))
		goto invalid;
	message->type = type;
	switch (type) {
	case FPLINUX_BRIGHTNESS_SET:
	case FPLINUX_BRIGHTNESS_SHOW:
	case FPLINUX_BRIGHTNESS_LEVEL:
		if (packet[1] > FPLINUX_BRIGHTNESS_LEVEL_MAX)
			goto invalid;
		message->level = packet[1];
		break;
	case FPLINUX_BRIGHTNESS_ERROR: {
		uint32_t error;

		memcpy(&error, packet + 1, sizeof(error));
		error = le32toh(error);
		if (!error || error > INT_MAX)
			goto invalid;
		message->error = (int)error;
		break;
	}
	}
	return 1;

invalid:
	errno = EPROTO;
	return -1;
}
