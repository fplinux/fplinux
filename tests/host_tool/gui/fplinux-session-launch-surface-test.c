/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "session-launch-surface.h"

static unsigned int failures;

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static int make_frame(size_t bytes)
{
	int fd = memfd_create("launchcard-test", MFD_CLOEXEC);
	uint16_t pixels[240];
	size_t written = 0;
	size_t index;

	if (fd < 0)
		abort();
	for (index = 0; index < sizeof(pixels) / sizeof(pixels[0]); ++index)
		pixels[index] = (uint16_t)(0x1100U + index);
	while (written < bytes) {
		size_t part = bytes - written;

		if (part > sizeof(pixels))
			part = sizeof(pixels);
		if (write(fd, pixels, part) != (ssize_t)part)
			abort();
		written += part;
	}
	return fd;
}

static void test_frame(int width, int height)
{
	struct fplinux_launch_frame frame = { 0 };
	int fd = make_frame(153600);

	check(FPLINUX_LAUNCH_FRAME_MAX_BYTES == 153600,
	      "both orientations keep the original RGB565 storage limit");
	check(fplinux_launch_frame_copy(&frame, fd, width, height),
	      "portrait and landscape tightly packed RGB565 frames are accepted");
	check(frame.width == width && frame.height == height &&
		      frame.bytes == 153600,
	      "only one RGB565 frame is retained");
	check(frame.pixels && frame.pixels[0] == 0x1100 &&
		      frame.pixels[239] == 0x11ef &&
		      frame.pixels[76799] == 0x11ef,
	      "pread copies every row even with the sender's fd at its end");
	check(lseek(fd, 0, SEEK_CUR) == 153600 && fcntl(fd, F_GETFD) >= 0,
	      "copying borrows the sender's fd without changing its offset");
	close(fd);
	check(frame.pixels && frame.pixels[76799] == 0x11ef,
	      "the copy survives the launcher fd closing and process exiting");
	fplinux_launch_frame_clear(&frame);
	check(!frame.pixels && !frame.bytes && !frame.width && !frame.height,
	      "ending a launch releases the frame and clears its dimensions");
	fplinux_launch_frame_clear(&frame);
	check(!frame.pixels, "clearing an absent frame is harmless");
}

static void test_invalid_frames(void)
{
	struct fplinux_launch_frame frame = { 0 };
	const int invalid[][2] = {
		{ 0, 128 },   { 128, 0 },   { -128, 128 }, { 128, -128 },
		{ 242, 320 }, { 240, 321 }, { 239, 320 },  { 321, 240 },
		{ 320, 241 }, { 320, 320 },
	};
	size_t index;
	int fd = make_frame(128 * 128 * 2);
	int short_fd = make_frame(128 * 128 * 2 - 2);
	int extra_fd = make_frame(128 * 128 * 2 + 2);
	int oversized_fd = make_frame(320 * 241 * 2);
	int pipe_fds[2];

	for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
		check(!fplinux_launch_frame_copy(&frame, fd, invalid[index][0],
						 invalid[index][1]),
		      "invalid dimensions reject without allocating a frame");
		check(!frame.pixels && !frame.bytes,
		      "invalid dimensions leave no retained pixels");
	}
	check(fplinux_launch_frame_copy(&frame, fd, 128, 128),
	      "a smaller profile replaces the absent frame");
	check(!fplinux_launch_frame_copy(&frame, short_fd, 128, 128) &&
		      !frame.pixels,
	      "a truncated snapshot rejects and releases the previous card");
	check(!fplinux_launch_frame_copy(&frame, extra_fd, 128, 128),
	      "snapshot metadata must match its exact byte size");
	check(!fplinux_launch_frame_copy(&frame, oversized_fd, 320, 241) &&
		      !frame.pixels && !frame.bytes,
	      "a landscape file exceeding the pixel budget is rejected");
	check(!fplinux_launch_frame_copy(&frame, -1, 128, 128),
	      "an unavailable snapshot leaves fallback rendering possible");
	if (pipe(pipe_fds) < 0)
		abort();
	check(!fplinux_launch_frame_copy(&frame, pipe_fds[0], 128, 128),
	      "nonregular fds reject without a blocking read");
	close(pipe_fds[0]);
	close(pipe_fds[1]);
	close(fd);
	close(short_fd);
	close(extra_fd);
	close(oversized_fd);
}

static void test_frame_draw_and_export(void)
{
	static const uint16_t expected[18] = {
		0x001f, 0xf800, 0x07e0, 0x001f, 0x001f, 0x001f,
		0x001f, 0xffff, 0xffe0, 0x001f, 0x001f, 0x001f,
		0x001f, 0x001f, 0x001f, 0x001f, 0x001f, 0x800f,
	};
	static const uint16_t expected_before_blend[18] = {
		0x001f, 0xf800, 0x07e0, 0x001f, 0x001f, 0x001f,
		0x001f, 0xffff, 0xffe0, 0x001f, 0x001f, 0x001f,
		0x001f, 0x001f, 0x001f, 0x001f, 0x001f, 0x001f,
	};
	static const uint32_t pixels[8] = {
		0xff0000ff, 0xff00ff00, 0xffff0000, 0xffabcdef,
		0xffffffff, 0xff00ffff, 0xffff0000, 0xffabcdef,
	};
	struct fplinux_launch_frame frame = { 0 };
	struct fplinux_launch_frame_rect clip = {
		.x = 1,
		.y = 0,
		.width = 3,
		.height = 2,
	};
	uint32_t half_red = 0x80000080;
	int fd;

	check(fplinux_launch_frame_fill(&frame, 6, 3, 0x123456),
	      "a bounded frame can be filled with an RGB color");
	check(frame.pixels && frame.pixels[0] == 0x11aa &&
		      frame.pixels[17] == 0x11aa,
	      "RGB input is quantized into literal RGB565 pixels");
	check(fplinux_launch_frame_fill(&frame, 6, 3, 0x0000ff),
	      "a replacement fill resets the frame to a literal blue base");
	fplinux_launch_frame_blend(&frame, pixels, 4, 2, 1, 0, clip);
	check(!memcmp(frame.pixels, expected_before_blend,
		      sizeof(expected_before_blend)),
	      "ABGR8888 source colors land at the placed pixels inside the clip");
	fplinux_launch_frame_blend(&frame, &half_red, 1, 1, 5, 2,
				   (struct fplinux_launch_frame_rect){
					   .x = 5,
					   .y = 2,
					   .width = 1,
					   .height = 1,
				   });
	check(frame.pixels[17] == 0x800f,
	      "premultiplied half-alpha red blends with the existing blue pixel");
	check(frame.pixels[4] == 0x001f && frame.pixels[10] == 0x001f &&
		      frame.pixels[12] == 0x001f && frame.pixels[16] == 0x001f,
	      "pixels outside placement and clip remain unchanged");
	fd = fplinux_launch_frame_fd(&frame);
	check(fd >= 0, "the packed frame exports through an owned descriptor");
	fplinux_launch_frame_clear(&frame);
	check(!frame.pixels && !frame.bytes && !frame.width && !frame.height,
	      "clearing the frame releases its owned pixel buffer");
	if (fd >= 0) {
		uint16_t exported[18] = { 0 };

		check(pread(fd, exported, sizeof(exported), 0) ==
				      (ssize_t)sizeof(exported) &&
			      !memcmp(exported, expected, sizeof(expected)),
		      "exported packed pixels remain available after frame clear");
		close(fd);
	}
}

static void test_packaged_frame(const char *path)
{
	struct fplinux_launch_frame frame = { 0 };

	check(fplinux_launch_frame_load(&frame, path, 2, 2),
	      "a packaged opaque RGB565 mark loads through the file boundary");
	check(frame.width == 2 && frame.height == 2 && frame.bytes == 8 &&
		      frame.pixels && frame.pixels[0] == 0xf800 &&
		      frame.pixels[1] == 0x07e0 && frame.pixels[2] == 0x001f &&
		      frame.pixels[3] == 0xffff,
	      "the mark retains literal red, green, blue and white RGB565 pixels");
	check(unlink(path) == 0 && frame.pixels && frame.pixels[3] == 0xffff,
	      "the compositor owns pixels after the source file and fd are gone");
	check(!fplinux_launch_frame_load(&frame, path, 2, 2) && !frame.pixels &&
		      !frame.bytes,
	      "a missing replacement cannot retain stale startup pixels");
	fplinux_launch_frame_clear(&frame);
}

static void test_spinner_profile(int size, int line_width)
{
	struct {
		uint32_t before;
		uint32_t pixels[FPLINUX_LAUNCH_SPINNER_PIXELS];
		uint32_t after;
	} image = { .before = 0x12345678, .after = 0x87654321 };
	uint32_t first[FPLINUX_LAUNCH_SPINNER_PIXELS];
	unsigned int phase;

	check(sizeof(image.pixels) == 4096,
	      "the largest spinner has a fixed 4 KiB buffer");
	for (phase = 0; phase < 8; ++phase) {
		unsigned int visible = 0;
		unsigned int bright = 0;
		size_t index;

		memset(image.pixels, 0xff, sizeof(image.pixels));
		check(fplinux_launch_spinner_draw(image.pixels, size,
						  line_width, phase, 0x804020),
		      "each normative profile's spinner geometry is accepted");
		check(image.before == 0x12345678 && image.after == 0x87654321,
		      "drawing never writes outside the tiny storage capacity");
		for (index = 0; index < FPLINUX_LAUNCH_SPINNER_PIXELS;
		     ++index) {
			uint32_t pixel = image.pixels[index];
			uint32_t alpha = pixel >> 24;

			if (alpha)
				++visible;
			if (alpha == 255)
				++bright;
			check(((pixel >> 16) & 255) <= alpha &&
				      ((pixel >> 8) & 255) <= alpha &&
				      (pixel & 255) <= alpha,
			      "spinner pixels use premultiplied ARGB for Pixman");
			check(alpha || !pixel,
			      "every pixel outside the ring is cleared transparent");
			if (index >= (size_t)size * (size_t)size)
				check(!pixel,
				      "pixels beyond the profile-sized image stay clear");
		}
		check(visible > 0 && bright > 0 && bright < visible,
		      "the ring contains a bright animated segment");
		check(!image.pixels[0] &&
			      !image.pixels[(size / 2) * size + size / 2],
		      "the ring leaves its corner and centre transparent");
		check(image.pixels[(line_width - 1) * size + size / 2] &&
			      !image.pixels[line_width * size + size / 2],
		      "the ring follows the profile's line width");
		if (!phase)
			memcpy(first, image.pixels, sizeof(first));
		else
			check(memcmp(first, image.pixels, sizeof(first)) != 0,
			      "animation advances without changing the card pixels");
	}
	check(fplinux_launch_spinner_draw(image.pixels, size, line_width, 8,
					  0x804020),
	      "the completed cycle is accepted");
	check(memcmp(first, image.pixels, sizeof(first)) == 0,
	      "spinner wraps after eight phases");
}

static void test_spinner(void)
{
	/* Distinct spinner sizes used by the supported screen profiles. */
	const int profiles[][2] = {
		{ 16, 2 },
		{ 24, 3 },
		{ 32, 4 },
	};
	const int invalid[][2] = {
		{ 0, 2 },   { -16, 2 }, { 33, 4 }, { 16, 0 },
		{ 16, -2 }, { 16, 8 },	{ 16, 9 },
	};
	uint32_t pixels[FPLINUX_LAUNCH_SPINNER_PIXELS];
	size_t index;

	for (index = 0; index < sizeof(profiles) / sizeof(profiles[0]); ++index)
		test_spinner_profile(profiles[index][0], profiles[index][1]);
	for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
		size_t pixel;

		memset(pixels, 0xff, sizeof(pixels));
		check(!fplinux_launch_spinner_draw(pixels, invalid[index][0],
						   invalid[index][1], 0,
						   0xffffff),
		      "invalid spinner geometry rejects without allocating");
		for (pixel = 0; pixel < FPLINUX_LAUNCH_SPINNER_PIXELS; ++pixel)
			check(!pixels[pixel],
			      "invalid geometry leaves no stale spinner pixels");
	}
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	test_frame(240, 320);
	test_frame(320, 240);
	test_invalid_frames();
	test_frame_draw_and_export();
	test_packaged_frame(argv[1]);
	test_spinner();
	if (failures)
		return EXIT_FAILURE;
	puts("launch frame and spinner: passed");
	return EXIT_SUCCESS;
}
