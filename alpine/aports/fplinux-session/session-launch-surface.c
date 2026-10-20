/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "session-launch-surface.h"

void fplinux_launch_frame_clear(struct fplinux_launch_frame *frame)
{
	free(frame->pixels);
	memset(frame, 0, sizeof(*frame));
}

static bool frame_size(int width, int height, size_t *bytes)
{
	if (width <= 0 || width > FPLINUX_UI_SESSION_FRAME_MAX_WIDTH ||
	    width % 2 || height <= 0 ||
	    height > FPLINUX_UI_SESSION_FRAME_MAX_HEIGHT)
		return false;
	*bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
	return *bytes <= FPLINUX_LAUNCH_FRAME_MAX_BYTES;
}

static uint16_t rgb565(uint32_t rgb)
{
	return (uint16_t)(((rgb >> 8) & 0xf800U) | ((rgb >> 5) & 0x07e0U) |
			  ((rgb >> 3) & 0x001fU));
}

bool fplinux_launch_frame_fill(struct fplinux_launch_frame *frame, int width,
			       int height, uint32_t rgb)
{
	size_t bytes;
	uint16_t colour = rgb565(rgb);

	fplinux_launch_frame_clear(frame);
	if (!frame_size(width, height, &bytes))
		return false;
	frame->pixels = malloc(bytes);
	if (!frame->pixels)
		return false;
	frame->bytes = bytes;
	frame->width = width;
	frame->height = height;
	for (size_t i = 0; i < bytes / sizeof(*frame->pixels); ++i)
		frame->pixels[i] = colour;
	return true;
}

void fplinux_launch_frame_blend(struct fplinux_launch_frame *frame,
				const uint32_t *pixels, int width, int height,
				int x, int y,
				struct fplinux_launch_frame_rect clip)
{
	if (!frame->pixels || !pixels || width <= 0 || height <= 0)
		return;
	for (int row = 0; row < height; ++row) {
		int dy = y + row;

		if (dy < 0 || dy >= frame->height || dy < clip.y ||
		    dy >= clip.y + clip.height)
			continue;
		for (int column = 0; column < width; ++column) {
			int dx = x + column;
			uint32_t pixel = pixels[(size_t)row * (size_t)width +
						(size_t)column];
			unsigned int alpha = pixel >> 24;
			unsigned int red = pixel & 255U;
			unsigned int green = (pixel >> 8) & 255U;
			unsigned int blue = (pixel >> 16) & 255U;
			uint16_t *target;
			uint16_t previous;

			if (dx < 0 || dx >= frame->width || dx < clip.x ||
			    dx >= clip.x + clip.width || !alpha)
				continue;
			target =
				&frame->pixels[(size_t)dy * (size_t)frame->width +
					       (size_t)dx];
			previous = *target;
			red += (((previous >> 11) * 255U / 31U) *
					(255U - alpha) +
				127U) /
			       255U;
			green += ((((previous >> 5) & 63U) * 255U / 63U) *
					  (255U - alpha) +
				  127U) /
				 255U;
			blue += (((previous & 31U) * 255U / 31U) *
					 (255U - alpha) +
				 127U) /
				255U;
			*target = rgb565((red << 16) | (green << 8) | blue);
		}
	}
}

int fplinux_launch_frame_fd(const struct fplinux_launch_frame *frame)
{
	size_t written = 0;
	int fd;

	if (!frame->pixels || !frame->bytes)
		return -1;
	fd = memfd_create("fplinux-frame", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	while (written < frame->bytes) {
		ssize_t count = write(fd, (const char *)frame->pixels + written,
				      frame->bytes - written);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			close(fd);
			return -1;
		}
		written += (size_t)count;
	}
	return fd;
}

bool fplinux_launch_frame_copy(struct fplinux_launch_frame *frame, int fd,
			       int width, int height)
{
	struct stat metadata;
	size_t bytes;
	size_t copied = 0;
	uint16_t *pixels;

	/* Never keep an old card as the fallback for another operation. */
	fplinux_launch_frame_clear(frame);
	/* Pixman requires its stride to be a multiple of four bytes. */
	if (fd < 0 || !frame_size(width, height, &bytes))
		return false;
	if (fstat(fd, &metadata) < 0 || !S_ISREG(metadata.st_mode) ||
	    metadata.st_size != (off_t)bytes)
		return false;
	pixels = malloc(bytes);
	if (!pixels)
		return false;
	while (copied < bytes) {
		ssize_t part = pread(fd, (char *)pixels + copied,
				     bytes - copied, (off_t)copied);

		if (part < 0 && errno == EINTR)
			continue;
		if (part <= 0) {
			free(pixels);
			return false;
		}
		copied += (size_t)part;
	}
	frame->pixels = pixels;
	frame->bytes = bytes;
	frame->width = width;
	frame->height = height;
	return true;
}

bool fplinux_launch_frame_load(struct fplinux_launch_frame *frame,
			       const char *path, int width, int height)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	bool loaded;

	if (fd < 0) {
		fplinux_launch_frame_clear(frame);
		return false;
	}
	loaded = fplinux_launch_frame_copy(frame, fd, width, height);
	close(fd);
	return loaded;
}

/* Clockwise octants, beginning with the segment just right of twelve. */
static unsigned int spinner_segment(int x, int y)
{
	if (y < 0) {
		if (x >= 0)
			return x < -y ? 0U : 1U;
		return -x < -y ? 7U : 6U;
	}
	if (x >= 0)
		return x > y ? 2U : 3U;
	return -x > y ? 5U : 4U;
}

bool fplinux_launch_spinner_draw(uint32_t pixels[FPLINUX_LAUNCH_SPINNER_PIXELS],
				 int size, int line_width, unsigned int phase,
				 uint32_t rgb)
{
	static const unsigned char alphas[FPLINUX_LAUNCH_SPINNER_PHASES] = {
		255, 216, 176, 136, 96, 64, 40, 24,
	};
	uint32_t colours[FPLINUX_LAUNCH_SPINNER_PHASES];
	int inner_radius;
	int inner_squared;
	int outer_squared;
	unsigned int segment;
	int y;

	memset(pixels, 0, FPLINUX_LAUNCH_SPINNER_PIXELS * sizeof(*pixels));
	if (size < 4 || size > FPLINUX_LAUNCH_SPINNER_MAX_SIZE ||
	    line_width <= 0 || line_width >= size / 2)
		return false;
	/* Doubled pixel centres avoid floating point and fractional radii. */
	inner_radius = size - 2 * line_width;
	inner_squared = inner_radius * inner_radius;
	outer_squared = size * size;
	phase %= FPLINUX_LAUNCH_SPINNER_PHASES;
	for (segment = 0; segment < FPLINUX_LAUNCH_SPINNER_PHASES; ++segment) {
		unsigned int alpha =
			alphas[(phase + FPLINUX_LAUNCH_SPINNER_PHASES - segment) %
			       FPLINUX_LAUNCH_SPINNER_PHASES];
		uint32_t red = (((rgb >> 16) & 255U) * alpha + 127U) / 255U;
		uint32_t green = (((rgb >> 8) & 255U) * alpha + 127U) / 255U;
		uint32_t blue = ((rgb & 255U) * alpha + 127U) / 255U;

		colours[segment] = (alpha << 24) | (red << 16) | (green << 8) |
				   blue;
	}
	for (y = 0; y < size; ++y) {
		int x;
		int dy = 2 * y + 1 - size;

		for (x = 0; x < size; ++x) {
			int dx = 2 * x + 1 - size;
			int radius_squared = dx * dx + dy * dy;

			if (radius_squared < inner_squared ||
			    radius_squared > outer_squared)
				continue;
			pixels[y * size + x] = colours[spinner_segment(dx, dy)];
		}
	}
	return true;
}
