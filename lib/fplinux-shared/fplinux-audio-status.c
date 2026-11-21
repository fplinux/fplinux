// SPDX-License-Identifier: LGPL-2.1-or-later
#define _DEFAULT_SOURCE
#include "fplinux-audio-status.h"

#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>

/* Type, levels, u32 rate, routes, nine flags, u64 clipped, u32 errno. */
enum {
	STATUS_SNAPSHOT = 1,
	STATUS_PACKET_BYTES = 30,
};

int fplinux_audio_status_send(int fd, const struct fplinux_audio_status *status)
{
	uint8_t packet[STATUS_PACKET_BYTES] = {
		[0] = STATUS_SNAPSHOT,	     [1] = status->media_volume,
		[2] = status->alarm_volume,  [7] = status->route,
		[8] = status->applied_route, [9] = status->speaker,
		[10] = status->headphones,   [11] = status->jack,
		[12] = status->plugged,	     [13] = status->fm,
		[14] = status->fm_active,    [15] = status->clock,
		[16] = status->foreground,   [17] = status->pcm,
	};
	uint32_t rate = htole32(status->rate);
	uint64_t clipped = htole64(status->clipped);
	uint32_t error = htole32(status->error);
	ssize_t bytes;

	memcpy(packet + 3, &rate, sizeof(rate));
	memcpy(packet + 18, &clipped, sizeof(clipped));
	memcpy(packet + 26, &error, sizeof(error));
	bytes = send(fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL);
	if (bytes == sizeof(packet))
		return 0;
	if (bytes >= 0)
		errno = EIO;
	return -1;
}

int fplinux_audio_status_receive(int fd, struct fplinux_audio_status *status)
{
	uint8_t packet[STATUS_PACKET_BYTES];
	ssize_t bytes =
		recv(fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
	uint32_t rate, error;
	uint64_t clipped;

	if (bytes < 0)
		return -1;
	if (bytes != sizeof(packet) || packet[0] != STATUS_SNAPSHOT ||
	    packet[1] > FPLINUX_AUDIO_VOLUME_MAX ||
	    packet[2] > FPLINUX_AUDIO_VOLUME_MAX ||
	    packet[7] > FPLINUX_AUDIO_ROUTE_HEADPHONES ||
	    packet[8] > FPLINUX_AUDIO_ROUTE_HEADPHONES)
		goto invalid;
	for (size_t i = 9; i <= 17; ++i)
		if (packet[i] > 1)
			goto invalid;
	memcpy(&rate, packet + 3, sizeof(rate));
	memcpy(&clipped, packet + 18, sizeof(clipped));
	memcpy(&error, packet + 26, sizeof(error));
	rate = le32toh(rate);
	error = le32toh(error);
	if (rate < FPLINUX_AUDIO_RATE_MIN || rate > FPLINUX_AUDIO_RATE_MAX ||
	    error > INT_MAX)
		goto invalid;
	*status = (struct fplinux_audio_status){
		.media_volume = packet[1],
		.alarm_volume = packet[2],
		.rate = rate,
		.route = packet[7],
		.applied_route = packet[8],
		.speaker = packet[9],
		.headphones = packet[10],
		.jack = packet[11],
		.plugged = packet[12],
		.fm = packet[13],
		.fm_active = packet[14],
		.clock = packet[15],
		.foreground = packet[16],
		.pcm = packet[17],
		.clipped = le64toh(clipped),
		.error = (int)error,
	};
	return 0;

invalid:
	errno = EPROTO;
	return -1;
}
