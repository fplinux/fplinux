/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "launcher-icons.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fplinux-ui.h"
#include "launcher-layout.h"

#define FPLINUX_LAUNCHER_ICON_FILE_BYTES 32768

static const uint8_t png_signature[8] = { 0x89, 'P',  'N',  'G',
					  '\r', '\n', 0x1a, '\n' };

void fplinux_launcher_icons_init(struct fplinux_launcher_icons *icons)
{
	icons->size_count = fplinux_launcher_icon_sizes(
		icons->sizes, FPLINUX_LAUNCHER_ICON_SIZE_COUNT);
	if (icons->size_count > FPLINUX_LAUNCHER_ICON_SIZE_COUNT)
		icons->size_count = FPLINUX_LAUNCHER_ICON_SIZE_COUNT;
}

void fplinux_launcher_icons_begin(struct fplinux_launcher_icons *reports)
{
	memcpy(reports->previous, reports->current, sizeof(reports->current));
	reports->previous_count = reports->current_count;
	reports->current_count = 0;
}

/*
 * Every catalogue load checks the icons again, and the session log lives in
 * RAM. A problem is logged when it was not reported by the previous load.
 */
void fplinux_launcher_icons_report(struct fplinux_launcher_icons *reports,
				   const char *app_id, const char *problem)
{
	char line[PATH_MAX + 128];
	uint32_t hash = 2166136261U;
	size_t capacity = sizeof(reports->current) / sizeof(*reports->current);

	snprintf(line, sizeof(line), "fplinux-launcher: %s: %s\n", app_id,
		 problem);
	for (const char *character = line; *character; ++character)
		hash = (hash ^ (unsigned char)*character) * 16777619U;
	/* Both icon sizes may meet the same file. */
	for (size_t i = 0; i < reports->current_count; ++i)
		if (reports->current[i] == hash)
			return;
	if (reports->current_count < capacity)
		reports->current[reports->current_count++] = hash;
	for (size_t i = 0; i < reports->previous_count; ++i)
		if (reports->previous[i] == hash)
			return;
	fputs(line, stderr);
}

static uint32_t read_be32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | bytes[3];
}

/* Icons follow the trust rule that the session applies to desktop entries. */
static const char *untrusted_icon_reason(const struct stat *status)
{
	if (!S_ISREG(status->st_mode))
		return "is not a regular file";
	if (status->st_uid != 0)
		return "is not owned by root";
	if (status->st_mode & (S_IWGRP | S_IWOTH))
		return "is writable by group or others";
	if (status->st_size > FPLINUX_LAUNCHER_ICON_FILE_BYTES)
		return "is larger than 32768 bytes";
	return NULL;
}

/* NULL with reason; *missing tells that the file does not exist. */
static uint8_t *read_icon_file(const char *path, size_t *bytes, bool *missing,
			       char *reason, size_t capacity)
{
	struct stat status;
	const char *untrusted;
	uint8_t *png;
	size_t size;
	size_t done = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);

	*missing = fd < 0 && errno == ENOENT;
	if (fd < 0) {
		snprintf(reason, capacity, "%s",
			 errno == ELOOP ? "is a symbolic link" :
					  strerror(errno));
		return NULL;
	}
	if (fstat(fd, &status) < 0) {
		snprintf(reason, capacity, "%s", strerror(errno));
		close(fd);
		return NULL;
	}
	untrusted = untrusted_icon_reason(&status);
	if (untrusted) {
		snprintf(reason, capacity, "%s", untrusted);
		close(fd);
		return NULL;
	}
	size = (size_t)status.st_size;
	png = malloc(size ? size : 1);
	if (!png) {
		snprintf(reason, capacity, "%s", strerror(ENOMEM));
		close(fd);
		return NULL;
	}
	while (done < size) {
		ssize_t count = read(fd, png + done, size - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		done += (size_t)count;
	}
	close(fd);
	if (done != size) {
		snprintf(reason, capacity, "changed or failed while reading");
		free(png);
		return NULL;
	}
	*bytes = size;
	return png;
}

/* The PNG header must give the size of the directory the file is in. */
static bool check_png_header(const uint8_t *png, size_t bytes, int size,
			     char *reason, size_t capacity)
{
	uint32_t width;
	uint32_t height;

	/* Signature, then IHDR: 13-byte length, type, width and height. */
	if (bytes < 24 || memcmp(png, png_signature, sizeof(png_signature)) ||
	    read_be32(png + 8) != 13 || memcmp(png + 12, "IHDR", 4)) {
		snprintf(reason, capacity, "is not a PNG file");
		return false;
	}
	width = read_be32(png + 16);
	height = read_be32(png + 20);
	if (width != (uint32_t)size || height != (uint32_t)size) {
		snprintf(reason, capacity,
			 "is %" PRIu32 "x%" PRIu32 ", expected %dx%d", width,
			 height, size, size);
		return false;
	}
	return true;
}

/*
 * The runtime decodes the checked PNG bytes once, scaling a larger file down
 * with its area filter, into an ARGB8888 buffer that redraws reuse; LVGL
 * keeps no reference to the PNG bytes.
 */
static lv_draw_buf_t *decode_icon(const uint8_t *png, size_t bytes,
				  int file_size, int size)
{
	const lv_image_dsc_t source = {
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_RAW_ALPHA,
			.w = (uint32_t)file_size,
			.h = (uint32_t)file_size,
		},
		.data_size = (uint32_t)bytes,
		.data = png,
	};

	return fplinux_ui_image_load(&source, size, size);
}

/*
 * The tile for size pixels: the file of exactly that size, else the next
 * larger one that is valid, scaled down. Problems are logged once.
 */
lv_draw_buf_t *fplinux_launcher_icons_load(struct fplinux_launcher_icons *icons,
					   const char *app_id, const char *name,
					   int size)
{
	char path[PATH_MAX];
	char reason[64];
	char problem[PATH_MAX + 80];
	bool found = false;

	for (size_t i = 0; i < icons->size_count; ++i) {
		int file_size = icons->sizes[i];
		lv_draw_buf_t *icon = NULL;
		uint8_t *png;
		size_t bytes;
		bool missing;

		if (file_size < size ||
		    snprintf(path, sizeof(path),
			     "/usr/share/icons/hicolor/%dx%d/apps/%s.png",
			     file_size, file_size, name) >= (int)sizeof(path))
			continue;
		png = read_icon_file(path, &bytes, &missing, reason,
				     sizeof(reason));
		if (missing)
			continue;
		found = true;
		if (png && check_png_header(png, bytes, file_size, reason,
					    sizeof(reason))) {
			icon = decode_icon(png, bytes, file_size, size);
			if (!icon)
				snprintf(
					reason, sizeof(reason), "%s",
					errno == EINVAL ?
						"cannot be decoded or is fully transparent" :
						strerror(errno));
		}
		free(png);
		if (icon)
			return icon;
		snprintf(problem, sizeof(problem), "ignoring icon %s: %s", path,
			 reason);
		fplinux_launcher_icons_report(icons, app_id, problem);
	}
	if (!found) {
		snprintf(problem, sizeof(problem),
			 "no icon %s.png of %d pixels or larger", name, size);
		fplinux_launcher_icons_report(icons, app_id, problem);
	}
	return NULL;
}
