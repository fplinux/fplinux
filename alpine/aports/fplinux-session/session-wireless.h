/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_WIRELESS_H
#define FPLINUX_SESSION_WIRELESS_H

#include <stdbool.h>

struct fplinux_session_wireless_state {
	bool supported;
	bool known;
	bool enabled;
	int error;
};

struct fplinux_session_wireless;

/* Discovery and all network operations run on the worker. */
struct fplinux_session_wireless *fplinux_session_wireless_create(void);
/* Remove the fd watch first. Waits for any accepted operation to finish. */
void fplinux_session_wireless_destroy(struct fplinux_session_wireless *wireless);
int fplinux_session_wireless_fd(const struct fplinux_session_wireless *wireless);
/* Drain notifications, then read the latest state and any result. */
void fplinux_session_wireless_dispatch(
	struct fplinux_session_wireless *wireless);
struct fplinux_session_wireless_state
fplinux_session_wireless_state(struct fplinux_session_wireless *wireless);
/* Returns an errno value. Only one request or unread result is permitted. */
int fplinux_session_wireless_request(struct fplinux_session_wireless *wireless,
				     bool enabled);
/* Returns true exactly once per accepted request; error is an errno value. */
bool fplinux_session_wireless_take_result(
	struct fplinux_session_wireless *wireless, int *error);

#endif
