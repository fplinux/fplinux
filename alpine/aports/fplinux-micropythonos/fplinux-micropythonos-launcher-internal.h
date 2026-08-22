/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MICROPYTHONOS_LAUNCHER_INTERNAL_H
#define FPLINUX_MICROPYTHONOS_LAUNCHER_INTERNAL_H

#include <linux/fb.h>
#include <stdbool.h>
#include <sys/types.h>

bool fplinux_micropythonos_launcher_has_two_pages(
	const struct fb_var_screeninfo *variable);
bool fplinux_micropythonos_launcher_framebuffer_valid(
	const struct fb_fix_screeninfo *fixed,
	const struct fb_var_screeninfo *variable);
int fplinux_micropythonos_launcher_acquire_session_lock(void);
pid_t fplinux_micropythonos_launcher_start_command(char *const command[]);
int fplinux_micropythonos_launcher_wait_for_command(pid_t child);

#endif
