/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_BRIGHTNESS_CLIENT_H
#define FPLINUX_BRIGHTNESS_CLIENT_H

#define FPLINUX_BRIGHTNESS_SOCKET "/run/fplinux-brightness.sock"

struct fplinux_brightness_client {
	int fd;
};

/* Initialize fd to -1 before connect. A null path selects the system socket. */
int fplinux_brightness_connect(struct fplinux_brightness_client *client,
			       const char *socket_path);
int fplinux_brightness_get(struct fplinux_brightness_client *client,
			   unsigned int *level);
int fplinux_brightness_set(struct fplinux_brightness_client *client,
			   unsigned int level);
int fplinux_brightness_claim(struct fplinux_brightness_client *client);
int fplinux_brightness_show(struct fplinux_brightness_client *client,
			    unsigned int level);
int fplinux_brightness_release(struct fplinux_brightness_client *client);
void fplinux_brightness_close(struct fplinux_brightness_client *client);

#endif
