// SPDX-License-Identifier: LGPL-2.1-or-later
#define _DEFAULT_SOURCE
#include "fplinux-audio-radio.h"

#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

static const uint8_t packet_sizes[] = {
	[FPLINUX_AUDIO_RADIO_SCAN] = 1,
	[FPLINUX_AUDIO_RADIO_PLAY] = 3,
	[FPLINUX_AUDIO_RADIO_READY] = 1,
	[FPLINUX_AUDIO_RADIO_ERROR] = 5,
};

static int frequency_valid(unsigned int frequency)
{
	return frequency >= FPLINUX_AUDIO_FM_LOW_100KHZ &&
	       frequency <= FPLINUX_AUDIO_FM_HIGH_100KHZ;
}

int fplinux_audio_radio_send(int fd, const struct fplinux_audio_radio *message)
{
	uint8_t packet[5];
	ssize_t bytes;
	size_t size;

	packet[0] = message->type;
	switch (message->type) {
	case FPLINUX_AUDIO_RADIO_PLAY: {
		uint16_t frequency;

		if (!frequency_valid(message->frequency))
			goto invalid;
		frequency = htole16(message->frequency);
		memcpy(packet + 1, &frequency, sizeof(frequency));
		break;
	}
	case FPLINUX_AUDIO_RADIO_ERROR: {
		uint32_t error;

		if (message->error <= 0)
			goto invalid;
		error = htole32(message->error);
		memcpy(packet + 1, &error, sizeof(error));
		break;
	}
	case FPLINUX_AUDIO_RADIO_SCAN:
	case FPLINUX_AUDIO_RADIO_READY:
		break;
	default:
		goto invalid;
	}
	size = packet_sizes[message->type];
	bytes = send(fd, packet, size, MSG_NOSIGNAL);
	if (bytes == (ssize_t)size)
		return 0;
	if (bytes >= 0)
		errno = EIO;
	return -1;

invalid:
	errno = EINVAL;
	return -1;
}

int fplinux_audio_radio_receive(int fd,
				enum fplinux_audio_radio_direction direction,
				struct fplinux_audio_radio *message)
{
	uint8_t packet[5];
	ssize_t bytes = recv(fd, packet, sizeof(packet), MSG_TRUNC);
	unsigned int type;

	if (bytes <= 0)
		return (int)bytes;
	type = packet[0];
	if (type >= sizeof(packet_sizes) || bytes != packet_sizes[type] ||
	    (direction == FPLINUX_AUDIO_RADIO_REQUEST) !=
		    (type <= FPLINUX_AUDIO_RADIO_PLAY))
		goto invalid;
	message->type = type;
	if (type == FPLINUX_AUDIO_RADIO_PLAY) {
		uint16_t frequency;

		memcpy(&frequency, packet + 1, sizeof(frequency));
		message->frequency = le16toh(frequency);
		if (!frequency_valid(message->frequency))
			goto invalid;
	}
	if (type == FPLINUX_AUDIO_RADIO_ERROR) {
		uint32_t error;

		memcpy(&error, packet + 1, sizeof(error));
		error = le32toh(error);
		if (!error || error > INT_MAX)
			goto invalid;
		message->error = (int)error;
	}
	return 1;

invalid:
	errno = EPROTO;
	return -1;
}
