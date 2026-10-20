// SPDX-License-Identifier: GPL-2.0-only
#include "showcase-hardware.h"

#include <stdio.h>

static bool report_brightness(const char *directory, const char *phase)
{
	char path[512];
	FILE *file;
	int brightness;
	int length;
	bool ok;

	length = snprintf(path, sizeof(path), "%s/brightness", directory);
	if (length < 0 || length >= (int)sizeof(path))
		return false;
	file = fopen(path, "r");
	if (!file)
		return false;
	ok = fscanf(file, "%d", &brightness) == 1;
	if (fclose(file) < 0)
		ok = false;
	if (ok)
		printf("%s %d\n", phase, brightness);
	return ok;
}

int main(int argc, char **argv)
{
	struct showcase_hardware hardware;
	struct armada_outputs outputs = { .keypad = true, .lcd_level = -1 };
	char error[256] = { 0 };
	int result = 1;

	if (argc != 2)
		return 2;
	if (!showcase_hardware_open(&hardware, argv[1], error, sizeof(error))) {
		fprintf(stderr, "cannot open Showcase hardware: %s\n", error);
		goto close;
	}
	if (!showcase_hardware_apply_outputs(&hardware, &outputs) ||
	    !report_brightness(argv[1], "ON"))
		goto close;
	outputs.keypad = false;
	if (!showcase_hardware_apply_outputs(&hardware, &outputs) ||
	    !report_brightness(argv[1], "OFF"))
		goto close;
	outputs.keypad = true;
	if (!showcase_hardware_apply_outputs(&hardware, &outputs))
		goto close;
	result = 0;

close:
	if (!showcase_hardware_close(&hardware))
		result = 1;
	return result;
}
