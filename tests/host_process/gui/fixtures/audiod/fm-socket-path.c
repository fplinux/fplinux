// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

int __real_connect(int fd, const struct sockaddr *address, socklen_t length);

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length)
{
	const struct sockaddr_un *system = (const void *)address;
	struct sockaddr_un fixture = { .sun_family = AF_UNIX };
	const char *path = getenv("FPLINUX_TEST_RADIO_SOCKET");

	assert(length == sizeof(*system) && system->sun_family == AF_UNIX);
	assert(!strcmp(system->sun_path, "/run/fplinux-audio/radio.sock"));
	assert(path && strlen(path) < sizeof(fixture.sun_path));
	strcpy(fixture.sun_path, path);
	return __real_connect(fd, (struct sockaddr *)&fixture, sizeof(fixture));
}
