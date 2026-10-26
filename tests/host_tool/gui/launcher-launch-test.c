/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "launcher-launch.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int failures;

static void check(bool condition, const char *text)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", text);
		++failures;
	}
}

int main(void)
{
	static uint16_t full_frame[320 * 320];
	uint16_t rows[] = { 0xf800, 0x07e0, 0xdead, 0x001f, 0xffff, 0xbeef };
	const uint16_t expected[] = { 0xf800, 0x07e0, 0x001f, 0xffff };
	uint16_t received[4] = { 0 };
	struct stat status;
	int fd = fplinux_launcher_launch_fd(rows, 3 * sizeof(*rows), 2, 2);

	check(fd >= 0, "a padded RGB565 frame becomes an anonymous file");
	if (fd >= 0) {
		check(fcntl(fd, F_GETFD) & FD_CLOEXEC,
		      "frame descriptor is close-on-exec");
		check(!fstat(fd, &status) && status.st_size == sizeof(expected),
		      "the file contains exactly packed rows");
		memset(rows, 0, sizeof(rows));
		check(pread(fd, received, sizeof(received), 0) ==
				      sizeof(received) &&
			      !memcmp(received, expected, sizeof(expected)),
		      "the copied image survives changes to the producer's draw buffer");
		close(fd);
	}
	check(fplinux_launcher_launch_fd(NULL, 4, 2, 2) < 0,
	      "missing pixels cannot create a descriptor");
	check(fplinux_launcher_launch_fd(rows, 2, 2, 2) < 0,
	      "a stride shorter than its row is rejected");
	check(fplinux_launcher_launch_fd(rows, 4, 0, 2) < 0,
	      "an empty image is rejected");
	check(fplinux_launcher_launch_fd(rows, 4, 2, 321) < 0,
	      "height cannot exceed the shipped screen's maximum");
	check(fplinux_launcher_launch_fd(rows, 642, 321, 2) < 0,
	      "width cannot exceed the shipped screen's maximum");
	full_frame[0] = 0x1234;
	full_frame[76799] = 0xabcd;
	fd = fplinux_launcher_launch_fd(full_frame, 640, 320, 240);
	check(fd >= 0, "a full horizontal launch frame is accepted");
	if (fd >= 0) {
		check(!fstat(fd, &status) && status.st_size == 153600,
		      "landscape keeps the original packed RGB565 byte budget");
		check(pread(fd, received, 2, 0) == 2 && received[0] == 0x1234 &&
			      pread(fd, received, 2, 153598) == 2 &&
			      received[0] == 0xabcd,
		      "the horizontal descriptor includes the first and last pixel");
		close(fd);
	}
	fd = fplinux_launcher_launch_fd(full_frame, 480, 240, 320);
	check(fd >= 0, "the original portrait launch frame remains accepted");
	if (fd >= 0)
		close(fd);
	check(fplinux_launcher_launch_fd(full_frame, 640, 320, 241) < 0,
	      "one extra horizontal row exceeds the original pixel budget");
	check(fplinux_launcher_launch_fd(full_frame, 484, 242, 320) < 0,
	      "an oversized portrait frame exceeds the same pixel budget");
	check(fplinux_launcher_launch_fd(full_frame, 640, 320, 320) < 0,
	      "individually bounded dimensions cannot enlarge retained storage");
	return failures ? 1 : 0;
}
