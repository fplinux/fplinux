/* SPDX-License-Identifier: GPL-2.0-only */

#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "session-bluetooth.h"
#include "session-core.h"

static void print_event(const struct fplinux_session_bluetooth_event *event)
{
	switch (event->state) {
	case FPLINUX_SESSION_BLUETOOTH_READY:
		puts("ready");
		break;
	case FPLINUX_SESSION_BLUETOOTH_OFF:
		puts("off");
		break;
	case FPLINUX_SESSION_BLUETOOTH_UNAVAILABLE:
		puts("unavailable");
		break;
	case FPLINUX_SESSION_BLUETOOTH_BUSY:
		puts("busy");
		break;
	case FPLINUX_SESSION_BLUETOOTH_OFFER:
		printf("offer %" PRIu32 " %s %s %" PRIu64 " %d\n", event->id,
		       event->peer, event->filename, event->total_bytes,
		       event->total_known);
		break;
	case FPLINUX_SESSION_BLUETOOTH_TRANSFERRING:
		printf("transferring %" PRIu32 "\n", event->id);
		break;
	case FPLINUX_SESSION_BLUETOOTH_COMPLETE:
		printf("complete %" PRIu32 " %s\n", event->id, event->path);
		break;
	case FPLINUX_SESSION_BLUETOOTH_FAILED:
		printf("failed %" PRIu32 "\n", event->id);
		break;
	case FPLINUX_SESSION_BLUETOOTH_CLEARED:
		printf("cleared %" PRIu32 "\n", event->id);
		break;
	}
	fflush(stdout);
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	setvbuf(stdin, NULL, _IONBF, 0);
	struct fplinux_session_options options = {
		.home = argv[1],
		.app_uid = getuid(),
		.app_gid = getgid(),
	};
	struct fplinux_session_bluetooth *bluetooth =
		fplinux_session_bluetooth_create(&options);
	if (bluetooth == NULL)
		return 2;
	struct pollfd descriptors[] = {
		{ .fd = STDIN_FILENO, .events = POLLIN },
		{ .fd = fplinux_session_bluetooth_fd(bluetooth),
		  .events = POLLIN },
	};
	int status = 0;
	while (poll(descriptors, 2, -1) > 0) {
		if (descriptors[0].revents) {
			char line[64];
			uint32_t id;
			int accept;
			if (!fgets(line, sizeof(line), stdin))
				break;
			if (sscanf(line, "%" SCNu32 " %d", &id, &accept) != 2 ||
			    !fplinux_session_bluetooth_decide(bluetooth, id,
							      accept)) {
				status = 2;
				break;
			}
		}
		if (descriptors[1].revents) {
			struct fplinux_session_bluetooth_event event;
			while (fplinux_session_bluetooth_receive(bluetooth,
								 &event))
				print_event(&event);
			if (fplinux_session_bluetooth_fd(bluetooth) < 0) {
				status = 2;
				break;
			}
		}
	}
	fplinux_session_bluetooth_destroy(bluetooth);
	return status;
}
