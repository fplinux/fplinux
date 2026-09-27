/* SPDX-License-Identifier: MIT */
#ifndef FPLINUX_KEYPAD_INTERNAL_H
#define FPLINUX_KEYPAD_INTERNAL_H

#include <stdbool.h>

/* The display owner releases input before yielding its VT. */
bool fplinux_keypad_set_active(bool active);

#endif
