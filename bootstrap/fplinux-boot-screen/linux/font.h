/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_BOOT_SCREEN_LINUX_FONT_H
#define FPLINUX_BOOT_SCREEN_LINUX_FONT_H

/* Freestanding declarations for the pinned Linux console font sources. */
struct font_desc {
	int idx;
	const char *name;
	unsigned int width, height;
	unsigned int charcount;
	const void *data;
	int pref;
};

struct font_data {
	unsigned int extra[4];
	const unsigned char data[];
} __attribute__((packed));

#define FONT7x14_IDX 4
#define FONT6x8_IDX 12

extern const struct font_desc font_6x8;
extern const struct font_desc font_7x14;

#endif
