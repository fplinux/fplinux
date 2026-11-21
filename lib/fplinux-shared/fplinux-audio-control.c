// SPDX-License-Identifier: LGPL-2.1-or-later
#define _DEFAULT_SOURCE
#include "fplinux-audio-control.h"

#include <endian.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#define CONTROL_PACKET_BYTES 10
#define CONTROL_ERROR_MAX 4095

static const uint8_t packet_sizes[] = {
	[FPLINUX_AUDIO_SET_VOLUME] = 3,
	[FPLINUX_AUDIO_SET_OUTPUT] = 2,
	[FPLINUX_AUDIO_CONTROL_APP] = 10,
	[FPLINUX_AUDIO_CONTROL_CLOCK] = 9,
	[FPLINUX_AUDIO_CONTROL_GONE] = 5,
	[FPLINUX_AUDIO_CONTROL_ELIGIBLE] = 6,
	[FPLINUX_AUDIO_CONTROL_SYNC] = 1,
	[FPLINUX_AUDIO_VOLUME_APPLIED] = 3,
	[FPLINUX_AUDIO_OUTPUT_APPLIED] = 2,
	[FPLINUX_AUDIO_OUTPUT_ERROR] = 3,
	[FPLINUX_AUDIO_CONTROL_ACTIVITY] = 10,
};

static const char *const route_names[] = {
	[FPLINUX_AUDIO_ROUTE_AUTO] = "AUTO",
	[FPLINUX_AUDIO_ROUTE_SPEAKER] = "SPEAKER",
	[FPLINUX_AUDIO_ROUTE_HEADPHONES] = "HEADPHONES",
};

static bool route_valid(enum fplinux_audio_route route)
{
	return (unsigned int)route <
	       sizeof(route_names) / sizeof(route_names[0]);
}

const char *fplinux_audio_route_name(enum fplinux_audio_route route)
{
	return route_valid(route) ? route_names[route] : NULL;
}

bool fplinux_audio_route_parse(const char *name,
			       enum fplinux_audio_route *route)
{
	for (unsigned int i = 0;
	     i < sizeof(route_names) / sizeof(route_names[0]); ++i)
		if (!strcmp(name, route_names[i])) {
			*route = i;
			return true;
		}
	return false;
}

static bool volume_valid(enum fplinux_audio_class class, unsigned int level)
{
	return (class == FPLINUX_AUDIO_MEDIA || class == FPLINUX_AUDIO_ALARM) &&
	       level <= FPLINUX_AUDIO_VOLUME_MAX;
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
	value = htole32(value);
	memcpy(bytes, &value, sizeof(value));
}

static uint32_t get_u32(const uint8_t *bytes)
{
	uint32_t value;

	memcpy(&value, bytes, sizeof(value));
	return le32toh(value);
}

static bool put_pid(uint8_t *bytes, pid_t pid)
{
	if (pid <= 0 || (uintmax_t)pid > INT32_MAX)
		return false;
	put_u32(bytes, (uint32_t)pid);
	return true;
}

static bool get_pid(const uint8_t *bytes, pid_t *pid)
{
	uint32_t value = get_u32(bytes);

	if (!value || value > INT32_MAX || (uintmax_t)(pid_t)value != value)
		return false;
	*pid = (pid_t)value;
	return true;
}

static bool put_uid(uint8_t *bytes, uid_t uid)
{
	uint32_t value = (uint32_t)uid;

	if ((uid_t)value != uid || uid == (uid_t)-1)
		return false;
	put_u32(bytes, value);
	return true;
}

static bool get_uid(const uint8_t *bytes, uid_t *uid)
{
	uint32_t value = get_u32(bytes);

	if (value == UINT32_MAX || (uintmax_t)(uid_t)value != value)
		return false;
	*uid = (uid_t)value;
	return true;
}

static bool encode(uint8_t *packet, const struct fplinux_audio_control *message)
{
	packet[0] = message->type;
	switch (message->type) {
	case FPLINUX_AUDIO_SET_VOLUME:
	case FPLINUX_AUDIO_VOLUME_APPLIED:
		packet[1] = message->volume.class;
		packet[2] = message->volume.level;
		return volume_valid(message->volume.class,
				    message->volume.level);
	case FPLINUX_AUDIO_SET_OUTPUT:
	case FPLINUX_AUDIO_OUTPUT_APPLIED:
		packet[1] = message->route;
		return route_valid(message->route);
	case FPLINUX_AUDIO_CONTROL_APP:
		packet[9] = message->app.background;
		return put_pid(packet + 1, message->app.pid) &&
		       put_uid(packet + 5, message->app.uid);
	case FPLINUX_AUDIO_CONTROL_CLOCK:
		return put_pid(packet + 1, message->clock.pid) &&
		       put_uid(packet + 5, message->clock.uid);
	case FPLINUX_AUDIO_CONTROL_GONE:
		return put_pid(packet + 1, message->gone);
	case FPLINUX_AUDIO_CONTROL_ELIGIBLE:
		packet[5] = message->eligible.enabled;
		return put_pid(packet + 1, message->eligible.pid);
	case FPLINUX_AUDIO_CONTROL_SYNC:
		return true;
	case FPLINUX_AUDIO_OUTPUT_ERROR:
		if (message->error <= 0 || message->error > CONTROL_ERROR_MAX)
			return false;
		packet[1] = message->error & 0xff;
		packet[2] = message->error >> 8;
		return true;
	case FPLINUX_AUDIO_CONTROL_ACTIVITY:
		packet[9] = message->activity.state;
		return (unsigned int)message->activity.state <=
			       FPLINUX_AUDIO_ACTIVITY_PLAYING &&
		       put_pid(packet + 1, message->activity.pid) &&
		       put_uid(packet + 5, message->activity.uid);
	}
	return false;
}

static bool decode(const uint8_t *packet, struct fplinux_audio_control *message)
{
	message->type = packet[0];
	switch (message->type) {
	case FPLINUX_AUDIO_SET_VOLUME:
	case FPLINUX_AUDIO_VOLUME_APPLIED:
		message->volume.class = packet[1];
		message->volume.level = packet[2];
		return volume_valid(message->volume.class,
				    message->volume.level);
	case FPLINUX_AUDIO_SET_OUTPUT:
	case FPLINUX_AUDIO_OUTPUT_APPLIED:
		message->route = packet[1];
		return route_valid(message->route);
	case FPLINUX_AUDIO_CONTROL_APP:
		message->app.background = packet[9];
		return packet[9] <= 1 &&
		       get_pid(packet + 1, &message->app.pid) &&
		       get_uid(packet + 5, &message->app.uid);
	case FPLINUX_AUDIO_CONTROL_CLOCK:
		return get_pid(packet + 1, &message->clock.pid) &&
		       get_uid(packet + 5, &message->clock.uid);
	case FPLINUX_AUDIO_CONTROL_GONE:
		return get_pid(packet + 1, &message->gone);
	case FPLINUX_AUDIO_CONTROL_ELIGIBLE:
		message->eligible.enabled = packet[5];
		return packet[5] <= 1 &&
		       get_pid(packet + 1, &message->eligible.pid);
	case FPLINUX_AUDIO_CONTROL_SYNC:
		return true;
	case FPLINUX_AUDIO_OUTPUT_ERROR:
		message->error = packet[1] | (packet[2] << 8);
		return message->error > 0 &&
		       message->error <= CONTROL_ERROR_MAX;
	case FPLINUX_AUDIO_CONTROL_ACTIVITY:
		message->activity.state = packet[9];
		return packet[9] <= FPLINUX_AUDIO_ACTIVITY_PLAYING &&
		       get_pid(packet + 1, &message->activity.pid) &&
		       get_uid(packet + 5, &message->activity.uid);
	}
	return false;
}

int fplinux_audio_control_send(int fd,
			       const struct fplinux_audio_control *message)
{
	uint8_t packet[CONTROL_PACKET_BYTES];
	ssize_t bytes;
	size_t size;

	if (!encode(packet, message)) {
		errno = EINVAL;
		return -1;
	}
	size = packet_sizes[message->type];
	do {
		bytes = send(fd, packet, size, MSG_DONTWAIT | MSG_NOSIGNAL);
	} while (bytes < 0 && errno == EINTR);
	if (bytes == (ssize_t)size)
		return 0;
	if (bytes >= 0)
		errno = EIO;
	return -1;
}

int fplinux_audio_control_receive(int fd,
				  enum fplinux_audio_control_direction direction,
				  struct fplinux_audio_control *message)
{
	uint8_t packet[CONTROL_PACKET_BYTES];
	ssize_t bytes =
		recv(fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
	unsigned int type;

	if (bytes <= 0)
		return (int)bytes;
	type = packet[0];
	if (type >= sizeof(packet_sizes) || bytes != packet_sizes[type] ||
	    (direction == FPLINUX_AUDIO_TO_SERVICE) !=
		    (type <= FPLINUX_AUDIO_CONTROL_SYNC) ||
	    !decode(packet, message)) {
		errno = EPROTO;
		return -1;
	}
	return 1;
}
