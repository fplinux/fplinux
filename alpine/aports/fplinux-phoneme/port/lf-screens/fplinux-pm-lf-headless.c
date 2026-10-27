/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * The app shell's services for natives (port/app/fplinux-pm-app.h) where
 * there is no app shell: weak definitions for the headless runner, which
 * links the same natives without a runtime connection. The app binary's
 * port/app/fplinux-pm-app.c defines them for real and takes precedence.
 */
#include "../app/fplinux-pm-app.h"

#include <stdbool.h>
#include <stddef.h>

__attribute__((weak)) struct fplinux_ui *fplinux_pm_app_ui(void)
{
	return NULL;
}

__attribute__((weak)) void fplinux_pm_app_loaded(void)
{
}

__attribute__((weak)) bool fplinux_pm_app_set_fullscreen(bool fullscreen)
{
	(void)fullscreen;
	return false;
}

__attribute__((weak)) void
fplinux_pm_app_set_resize_handler(fplinux_pm_app_resize_fn handler,
				  void *context)
{
	(void)handler;
	(void)context;
}
