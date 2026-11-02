/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PM_STORE_H
#define FPLINUX_PM_STORE_H

struct fplinux_pm_suite;

/*
 * Creates a private RMS directory under an existing absolute state root.
 * Complete suite identity uses vendor and name; incomplete identity uses the
 * canonical JAR path. Returns a malloc-owned path, or NULL with errno set.
 * The caller frees the path but retains the directory and its contents.
 */
char *fplinux_pm_store_open(const char *state_root,
			    const struct fplinux_pm_suite *suite,
			    const char *jar_path);

#endif
