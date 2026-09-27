/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Phone key codes, shared by the device trees, the kernel and phone userspace.
 *
 * These are standard Linux input codes. Applications interpret their phone
 * roles only on a keypad source; keyboard F13/F14 keep their keyboard roles.
 */

#ifndef FPLINUX_KEYPAD_H
#define FPLINUX_KEYPAD_H

#ifdef __DTS__
#include <dt-bindings/input/linux-event-codes.h>
#else
#include <linux/input-event-codes.h>
#endif

#define FPLINUX_KEY_0 KEY_NUMERIC_0
#define FPLINUX_KEY_1 KEY_NUMERIC_1
#define FPLINUX_KEY_2 KEY_NUMERIC_2
#define FPLINUX_KEY_3 KEY_NUMERIC_3
#define FPLINUX_KEY_4 KEY_NUMERIC_4
#define FPLINUX_KEY_5 KEY_NUMERIC_5
#define FPLINUX_KEY_6 KEY_NUMERIC_6
#define FPLINUX_KEY_7 KEY_NUMERIC_7
#define FPLINUX_KEY_8 KEY_NUMERIC_8
#define FPLINUX_KEY_9 KEY_NUMERIC_9
#define FPLINUX_KEY_STAR KEY_NUMERIC_STAR
#define FPLINUX_KEY_POUND KEY_NUMERIC_POUND

#define FPLINUX_KEY_UP KEY_UP
#define FPLINUX_KEY_DOWN KEY_DOWN
#define FPLINUX_KEY_LEFT KEY_LEFT
#define FPLINUX_KEY_RIGHT KEY_RIGHT
#define FPLINUX_KEY_OK KEY_OK
#define FPLINUX_KEY_SOFT_LEFT KEY_F13
#define FPLINUX_KEY_SOFT_RIGHT KEY_F14
/* Dial key, the green handset key. */
#define FPLINUX_KEY_CALL KEY_PICKUP_PHONE
/* Power key; holding it for five seconds requests power-off. */
#define FPLINUX_KEY_POWER KEY_POWER

#endif
