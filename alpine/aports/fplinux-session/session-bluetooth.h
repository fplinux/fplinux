/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_BLUETOOTH_H
#define FPLINUX_SESSION_BLUETOOTH_H

#include "fplinux-bluetooth-receive.h"

struct fplinux_session_options;
struct fplinux_session_bluetooth;

struct fplinux_session_bluetooth *
fplinux_session_bluetooth_create(const struct fplinux_session_options *options);
void fplinux_session_bluetooth_destroy(
	struct fplinux_session_bluetooth *bluetooth);
int fplinux_session_bluetooth_fd(
	const struct fplinux_session_bluetooth *bluetooth);
bool fplinux_session_bluetooth_receive(
	struct fplinux_session_bluetooth *bluetooth,
	struct fplinux_session_bluetooth_event *event);
bool fplinux_session_bluetooth_decide(
	struct fplinux_session_bluetooth *bluetooth, uint32_t id, bool accept);

#endif
