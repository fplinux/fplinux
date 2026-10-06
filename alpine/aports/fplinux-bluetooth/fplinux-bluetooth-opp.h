// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_BLUETOOTH_OPP_H
#define FPLINUX_BLUETOOTH_OPP_H

#include <signal.h>

int fplinux_bluetooth_opp_send(const char *peer, const char *file,
			       const volatile sig_atomic_t *interrupted);
int fplinux_bluetooth_opp_receive(const char *peer, const char *directory,
				  int seconds,
				  const volatile sig_atomic_t *interrupted);

#endif
