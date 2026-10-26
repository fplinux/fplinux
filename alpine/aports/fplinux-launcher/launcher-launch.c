/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "launcher-launch.h"
#include "fplinux-ui-session.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

int fplinux_launcher_launch_fd(const void *pixels, size_t stride, int width,
			       int height)
{
	int fd;
	size_t row;

	if (!pixels || width <= 0 || height <= 0 ||
	    width > FPLINUX_UI_SESSION_FRAME_MAX_WIDTH ||
	    height > FPLINUX_UI_SESSION_FRAME_MAX_HEIGHT ||
	    (size_t)width * (size_t)height >
		    FPLINUX_UI_SESSION_FRAME_MAX_PIXELS ||
	    stride < (size_t)width * sizeof(uint16_t)) {
		errno = EINVAL;
		return -1;
	}
	row = (size_t)width * sizeof(uint16_t);
	fd = memfd_create("fplinux-launch", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	for (int y = 0; y < height; ++y) {
		const uint8_t *source =
			(const uint8_t *)pixels + (size_t)y * stride;
		size_t written = 0;

		while (written < row) {
			ssize_t count =
				write(fd, source + written, row - written);

			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0) {
				int error = count ? errno : EIO;

				close(fd);
				errno = error;
				return -1;
			}
			written += (size_t)count;
		}
	}
	return fd;
}
