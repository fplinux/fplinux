/* SPDX-License-Identifier: GPL-2.0-only */
#include "screen-frame.h"

#include <stdio.h>

bool screen_frame_write(const char *path, const uint16_t *pixels,
			unsigned int width, unsigned int height,
			size_t stride_bytes)
{
	FILE *file = fopen(path, "wb");
	bool written = file != NULL;

	for (unsigned int row = 0; written && row < height; ++row) {
		const uint16_t *line =
			(const uint16_t *)((const unsigned char *)pixels +
					   row * stride_bytes);

		for (unsigned int column = 0; written && column < width;
		     ++column) {
			unsigned char bytes[2] = { line[column] & 0xffU,
						   line[column] >> 8 };

			written = fwrite(bytes, sizeof(bytes), 1, file) == 1;
		}
	}
	if (file && fclose(file))
		written = false;
	if (!written)
		fprintf(stderr, "cannot write %s\n", path);
	return written;
}

bool screen_frame_read(const char *path, uint16_t *pixels, unsigned int width,
		       unsigned int height)
{
	FILE *file = fopen(path, "rb");
	bool read = file != NULL;

	for (size_t index = 0; read && index < (size_t)width * height;
	     ++index) {
		unsigned char bytes[2];

		read = fread(bytes, sizeof(bytes), 1, file) == 1;
		pixels[index] = (uint16_t)(bytes[0] | bytes[1] << 8);
	}
	if (read && fgetc(file) != EOF)
		read = false;
	if (file)
		fclose(file);
	if (!read)
		fprintf(stderr, "cannot read a %ux%u RGB565 frame from %s\n",
			width, height, path);
	return read;
}
