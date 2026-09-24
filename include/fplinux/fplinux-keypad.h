/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Phone key codes, shared by the device trees, the kernel and phone userspace.
 *
 * Every phone key has its own code, so an application can tell it from any
 * keyboard key. The codes are unassigned in the pinned kernel and lie in the
 * ranges that libinput delivers as keys. The kernel console does not translate
 * them.
 *
 * Further phone keys take the reserved codes 0x23c-0x23f, then 0x1e6-0x1f0.
 */

#ifndef FPLINUX_KEYPAD_H
#define FPLINUX_KEYPAD_H

#define FPLINUX_KEY_0 0x1c4
#define FPLINUX_KEY_1 0x1c5
#define FPLINUX_KEY_2 0x1c6
#define FPLINUX_KEY_3 0x1c7
#define FPLINUX_KEY_4 0x1c8
#define FPLINUX_KEY_5 0x1c9
#define FPLINUX_KEY_6 0x1ca
#define FPLINUX_KEY_7 0x1cb
#define FPLINUX_KEY_8 0x1cc
#define FPLINUX_KEY_9 0x1cd
#define FPLINUX_KEY_STAR 0x1ce
#define FPLINUX_KEY_POUND 0x1cf

#define FPLINUX_KEY_UP 0x233
#define FPLINUX_KEY_DOWN 0x234
#define FPLINUX_KEY_LEFT 0x235
#define FPLINUX_KEY_RIGHT 0x236
#define FPLINUX_KEY_OK 0x237
#define FPLINUX_KEY_SOFT_LEFT 0x238
#define FPLINUX_KEY_SOFT_RIGHT 0x239
/* Dial key, the green handset key. */
#define FPLINUX_KEY_CALL 0x23a
/* Power key; holding it for five seconds requests power-off. */
#define FPLINUX_KEY_POWER 0x23b

#endif
