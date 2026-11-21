/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PM_STORE_H
#define FPLINUX_PM_STORE_H

#include <stdbool.h>

struct fplinux_pm_suite;

/* Forms the settings and RMS key from the suite identity or JAR path. */
#define FPLINUX_PM_STORE_KEY_SIZE 65
int fplinux_pm_store_key(const struct fplinux_pm_suite *suite,
			 const char *jar_path,
			 char key[FPLINUX_PM_STORE_KEY_SIZE]);

struct fplinux_pm_store {
	char prefs_key[FPLINUX_PM_STORE_KEY_SIZE];
	char *rms_path;
};

/*
 * Creates a private RMS directory under an existing absolute state root.
 * On success the caller owns rms_path. On failure returns false with errno set.
 */
bool fplinux_pm_store_open(const char *state_root,
			   const struct fplinux_pm_suite *suite,
			   const char *jar_path,
			   struct fplinux_pm_store *output);

#endif
