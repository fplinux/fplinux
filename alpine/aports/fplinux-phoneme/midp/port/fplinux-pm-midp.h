/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The MIDP core of fplinux-phoneme as seen by the VM glue (vm/).
 *
 * Call order on the VM thread: fplinux_pm_midp_init() before JVM_Start (or
 * the first slave-mode slice), fplinux_pm_midp_cleanup() after JVM_CleanUp.
 * One MIDP instance per process; init after cleanup is allowed.
 */
#ifndef FPLINUX_PM_MIDP_H
#define FPLINUX_PM_MIDP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only MIDP home: configuration and resources installed by the package. */
#define FPLINUX_PM_MIDP_HOME "/usr/share/fplinux-phoneme"

/*
 * Prepare everything MIDP needs before the VM starts: storage roots
 * (storage_dir is the writable per-run directory for record stores and other
 * MIDP files; NULL leaves storage unusable, so RecordStore.openRecordStore
 * fails), the static MIDP properties and the VM system properties
 * (microedition.platform, .profiles, .configuration, .locale, ...), the
 * native event queue, the screen buffer and the timezone. Returns false,
 * with a message on stderr, when something cannot be set up; nothing needs
 * cleaning up then.
 */
bool fplinux_pm_midp_init(const char *storage_dir);

/* Release what fplinux_pm_midp_init() set up. Safe to call twice. */
void fplinux_pm_midp_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
