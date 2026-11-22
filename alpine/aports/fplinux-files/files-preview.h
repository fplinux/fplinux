/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FILES_PREVIEW_H
#define FPLINUX_FILES_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>

struct files_preview;

struct files_preview_image {
	const unsigned char *data;
	size_t bytes;
	unsigned int width;
	unsigned int height;
};

struct files_preview *files_preview_new(void);
/* Teardown may wait for an interrupted helper; reset is nonblocking. */
void files_preview_free(struct files_preview *preview);
void files_preview_reset(struct files_preview *preview);
bool files_preview_busy(const struct files_preview *preview);
/* Indices belong to one directory snapshot. The returned image is borrowed
 * until the next call that mutates the preview state.
 */
const struct files_preview_image *
files_preview_request(struct files_preview *preview, size_t index,
		      const char *program, const char *path);
/* Call periodically while busy. Reports an index after success or failure. */
bool files_preview_poll(struct files_preview *preview, size_t *completed_index);

#endif
