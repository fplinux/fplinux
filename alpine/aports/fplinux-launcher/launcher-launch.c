/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "launcher-launch.h"
#include "fplinux-ui-session.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void fplinux_launcher_backdrop_clear(struct fplinux_launcher_backdrop *backdrop)
{
	if (backdrop->pixels)
		munmap(backdrop->pixels, backdrop->bytes);
	memset(backdrop, 0, sizeof(*backdrop));
}

bool fplinux_launcher_backdrop_map(struct fplinux_launcher_backdrop *backdrop,
				   int fd, int width, int height)
{
	struct stat metadata;
	size_t bytes;
	void *pixels;

	fplinux_launcher_backdrop_clear(backdrop);
	if (fd < 0 || width <= 0 || height <= 0 || width % 2 ||
	    width > FPLINUX_UI_SESSION_FRAME_MAX_WIDTH ||
	    height > FPLINUX_UI_SESSION_FRAME_MAX_HEIGHT ||
	    (size_t)width * (size_t)height >
		    FPLINUX_UI_SESSION_FRAME_MAX_PIXELS)
		return false;
	bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
	if (fstat(fd, &metadata) < 0 || !S_ISREG(metadata.st_mode) ||
	    metadata.st_size != (off_t)bytes)
		return false;
	pixels = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
	if (pixels == MAP_FAILED)
		return false;
	backdrop->pixels = pixels;
	backdrop->bytes = bytes;
	backdrop->width = width;
	backdrop->height = height;
	return true;
}

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
