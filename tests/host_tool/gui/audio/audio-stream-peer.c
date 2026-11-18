// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "fplinux-audio-protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct fplinux_audio_message message;
	struct fplinux_audio_ring *ring = NULL;
	char command[64];
	uint32_t id = 0;
	int fd;

	assert(argc == 2);
	setvbuf(stdout, NULL, _IOLBF, 0);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	assert(fd >= 0);
	strcpy(address.sun_path, argv[1]);
	assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	printf("%d\n", (int)getpid());
	while (fgets(command, sizeof(command), stdin)) {
		unsigned int count, value;

		if (!strcmp(command, "OPEN\n")) {
			union {
				struct cmsghdr header;
				char bytes[CMSG_SPACE(sizeof(int))];
			} control;
			struct iovec iov = { &message, sizeof(message) };
			struct msghdr received = {
				.msg_iov = &iov,
				.msg_iovlen = 1,
				.msg_control = control.bytes,
				.msg_controllen = sizeof(control),
			};
			int mapping;

			message = (struct fplinux_audio_message){
				.type = FPLINUX_AUDIO_HELLO,
				.version = 1,
			};
			assert(send(fd, &message, sizeof(message), 0) ==
			       (ssize_t)sizeof(message));
			message = (struct fplinux_audio_message){
				.type = FPLINUX_AUDIO_OPEN,
				.rate = 48000,
				.channels = 1,
			};
			assert(send(fd, &message, sizeof(message), 0) ==
			       (ssize_t)sizeof(message));
			assert(recvmsg(fd, &received, 0) ==
			       (ssize_t)sizeof(message));
			assert(message.type == FPLINUX_AUDIO_OPENED);
			id = message.stream;
			memcpy(&mapping, CMSG_DATA(CMSG_FIRSTHDR(&received)),
			       sizeof(mapping));
			ring = mmap(NULL, 32832, PROT_READ | PROT_WRITE,
				    MAP_SHARED, mapping, 0);
			assert(ring != MAP_FAILED);
			close(mapping);
		} else if (sscanf(command, "WRITE %u %u", &count, &value) ==
			   2) {
			uint32_t write = atomic_load(&ring->write);
			uint32_t read = atomic_load(&ring->read);
			int16_t *frames = fplinux_audio_ring_frames(ring);

			assert(count <= ring->capacity - (write - read));
			for (unsigned int i = 0; i < count; ++i)
				frames[(write + i) & (ring->capacity - 1)] =
					(int16_t)value;
			atomic_store(&ring->write, write + count);
			message = (struct fplinux_audio_message){
				.type = FPLINUX_AUDIO_KICK, .stream = id
			};
			assert(send(fd, &message, sizeof(message), 0) ==
			       (ssize_t)sizeof(message));
		} else if (sscanf(command, "PAUSE %u", &value) == 1) {
			message = (struct fplinux_audio_message){
				.type = FPLINUX_AUDIO_PAUSE,
				.stream = id,
				.flags = value,
			};
			assert(send(fd, &message, sizeof(message), 0) ==
			       (ssize_t)sizeof(message));
		} else if (!strcmp(command, "STATE\n")) {
			printf("%u %u %u %u\n", atomic_load(&ring->read),
			       atomic_load(&ring->heard),
			       atomic_load(&ring->state),
			       atomic_load(&ring->reason));
			continue;
		} else {
			abort();
		}
		puts("OK");
	}
	if (ring)
		munmap(ring, 32832);
	close(fd);
	return 0;
}
