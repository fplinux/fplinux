/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "files-preview.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

static size_t wait_for_result(struct files_preview *preview)
{
	const struct timespec interval = { .tv_nsec = 10000000 };
	size_t index;

	for (unsigned int attempt = 0; attempt < 500; ++attempt) {
		assert(files_preview_busy(preview));
		if (files_preview_poll(preview, &index))
			return index;
		nanosleep(&interval, NULL);
	}
	fputs("preview did not finish\n", stderr);
	exit(1);
}

static void check_image(const struct files_preview_image *image,
			const char *path, unsigned long width,
			unsigned long height)
{
	FILE *input;
	size_t offset = 0;
	int byte;

	if (!width) {
		assert(!image);
		return;
	}
	assert(image && image->width == width && image->height == height);
	input = fopen(path, "rb");
	assert(input);
	while ((byte = fgetc(input)) != EOF) {
		assert(offset < image->bytes && image->data[offset] == byte);
		++offset;
	}
	assert(!ferror(input) && offset == image->bytes);
	fclose(input);
}

static void check_preview(struct files_preview *preview, const char *program,
			  const char *path, unsigned long width,
			  unsigned long height)
{
	const size_t index = 4;
	size_t completed_index;

	assert(!files_preview_request(preview, index, program, path));
	assert(!files_preview_request(preview, index, program, path));
	completed_index = wait_for_result(preview);
	assert(completed_index == index);
	check_image(files_preview_request(preview, index, program, path), path,
		    width, height);
	assert(!files_preview_busy(preview));
	assert(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
	files_preview_reset(preview);
	assert(!files_preview_request(preview, index, program, path));
	assert(files_preview_busy(preview));
}

static void check_cancel(struct files_preview *preview, const char *waiting,
			 const char *producer, const char *path)
{
	const size_t index = 4;
	size_t completed_index;

	assert(!files_preview_request(preview, 5, waiting, path));
	assert(!files_preview_request(preview, index, waiting, path));
	assert(!files_preview_poll(preview, &completed_index));
	files_preview_reset(preview);
	assert(!files_preview_request(preview, index, producer, path));
	completed_index = wait_for_result(preview);
	assert(completed_index == index);
	check_image(files_preview_request(preview, index, producer, path), path,
		    2, 1);
	assert(!files_preview_busy(preview));
	assert(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
}

int main(int argc, char **argv)
{
	struct files_preview *preview = files_preview_new();

	assert(argc >= 2 && preview);
	if (strcmp(argv[1], "cancel") == 0) {
		assert(argc == 5);
		check_cancel(preview, argv[2], argv[3], argv[4]);
	} else {
		assert(argc == 6);
		check_preview(preview, argv[2], argv[3],
			      strtoul(argv[4], NULL, 10),
			      strtoul(argv[5], NULL, 10));
	}
	files_preview_free(preview);
	return 0;
}
