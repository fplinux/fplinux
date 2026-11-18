/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PM_MEDIA_H
#define FPLINUX_PM_MEDIA_H

#include <stdbool.h>
#include <jvmspi.h>

void fplinux_pm_media_set_wakeup(void (*wakeup)(void *), void *context);
void fplinux_pm_media_set_visible(bool visible);
void fplinux_pm_media_cancel(void);
/* Called inside the VM thread, never from a UI descriptor callback. */
void fplinux_pm_media_check_events(JVMSPI_BlockedThreadInfo *threads,
				   int count);
void fplinux_pm_media_cleanup(void);

#endif
