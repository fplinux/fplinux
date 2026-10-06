// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_BLUETOOTH_PAN_H
#define FPLINUX_BLUETOOTH_PAN_H

#include <signal.h>

int fplinux_bluetooth_pan_connect(const char *peer,
				  const volatile sig_atomic_t *interrupted);

#endif
