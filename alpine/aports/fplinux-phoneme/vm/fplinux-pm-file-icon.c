/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-pm-vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define ICON_MAX_BYTES 65536U
#define ICON_MAX_DIMENSION 128U
#define PNG_IHDR_BYTES 13U

static uint32_t read_be32(const unsigned char *data)
{
	return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
	       (uint32_t)data[2] << 8 | data[3];
}

static bool has_bounded_png_header(const unsigned char *png, size_t size)
{
	static const unsigned char signature[] = "\x89PNG\r\n\x1a\n";
	const size_t signature_bytes = sizeof(signature) - 1;
	const unsigned char *ihdr;
	uint32_t width, height;

	if (size < signature_bytes + 12 + PNG_IHDR_BYTES ||
	    memcmp(png, signature, signature_bytes))
		return false;
	ihdr = png + signature_bytes + 4;
	if (read_be32(png + signature_bytes) != PNG_IHDR_BYTES ||
	    memcmp(ihdr, "IHDR", 4) ||
	    crc32(0, ihdr, 4 + PNG_IHDR_BYTES) !=
		    read_be32(ihdr + 4 + PNG_IHDR_BYTES))
		return false;
	width = read_be32(ihdr + 4);
	height = read_be32(ihdr + 8);
	return width && width <= ICON_MAX_DIMENSION && height &&
	       height <= ICON_MAX_DIMENSION && ihdr[12] <= 8;
}

static unsigned char *read_icon(const char *jar_path, const char *entry,
				size_t *size)
{
	unsigned char *png;

	if (!entry[0] ||
	    fplinux_pm_jar_read_entry(jar_path, entry, ICON_MAX_BYTES, &png,
				      size) != FPLINUX_PM_JAR_OK)
		return NULL;
	if (has_bounded_png_header(png, *size))
		return png;
	free(png);
	return NULL;
}

int main(int argc, char **argv)
{
	struct fplinux_pm_suite suite;
	const char *extension;
	unsigned char *png;
	size_t size;
	bool written;

	if (argc != 2 || argv[1][0] != '/')
		return 2;
	extension = strrchr(argv[1], '.');
	if (!extension ||
	    (strcasecmp(extension, ".jar") && strcasecmp(extension, ".jad")))
		return 1;
	if (fplinux_pm_suite_open(argv[1], &suite, NULL) != FPLINUX_PM_JAR_OK)
		return 1;
	png = read_icon(suite.jar_path, suite.icon, &size);
	if (!png)
		png = read_icon(suite.jar_path, suite.midlets[0].icon, &size);
	fplinux_pm_suite_cleanup(&suite);
	if (!png)
		return 1;
	written = fwrite(png, 1, size, stdout) == size && fflush(stdout) == 0;
	free(png);
	return written ? 0 : 1;
}
