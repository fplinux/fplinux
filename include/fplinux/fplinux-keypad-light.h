// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_KEYPAD_LIGHT_H
#define FPLINUX_KEYPAD_LIGHT_H

#include <stdbool.h>
#include <stddef.h>

#define FPLINUX_KEYPAD_LIGHT_PATH_BYTES 512U

struct fplinux_keypad_light {
	char directory[FPLINUX_KEYPAD_LIGHT_PATH_BYTES];
	char *original_trigger;
	int original_brightness;
	int maximum;
	bool claimed;
};

/* Find the unique kbd_backlight function without changing its state. */
int fplinux_keypad_light_find(char *directory, size_t size,
			      const char *leds_directory);
/*
 * Open borrows a light, disables its trigger and switches it off. The caller
 * supplies a zero-initialized or closed object. Failure restores any borrowed
 * state; close also accepts an unopened object. A missing trigger is allowed.
 */
int fplinux_keypad_light_open(struct fplinux_keypad_light *light,
			      const char *directory);
/* ON uses the LED's advertised maximum; OFF uses zero. */
int fplinux_keypad_light_set(struct fplinux_keypad_light *light, bool on);
/* Restore brightness, then the original trigger, and release owned state. */
int fplinux_keypad_light_close(struct fplinux_keypad_light *light);

#endif
