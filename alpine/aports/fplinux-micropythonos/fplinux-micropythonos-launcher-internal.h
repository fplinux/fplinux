/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MICROPYTHONOS_LAUNCHER_INTERNAL_H
#define FPLINUX_MICROPYTHONOS_LAUNCHER_INTERNAL_H

#include <sys/types.h>

int fplinux_micropythonos_launcher_acquire_session_lock(void);
pid_t fplinux_micropythonos_launcher_start_command(char *const command[]);
int fplinux_micropythonos_launcher_wait_for_command(pid_t child);

#endif
