/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PM_NETWORK_H
#define FPLINUX_PM_NETWORK_H

#include <stdbool.h>
#include <jvmspi.h>

int fplinux_pm_network_lookup_start(char *hostname, unsigned char *address,
				    int capacity, int *length, void **handle,
				    void **context);
int fplinux_pm_network_lookup_finish(unsigned char *address, int capacity,
				     int *length, void *handle, void *context);

/* Called only on the VM thread, with a fresh SNI blocked-thread snapshot. */
bool fplinux_pm_network_check_events(JVMSPI_BlockedThreadInfo *threads,
				     int count);
bool fplinux_pm_network_has_waiters(JVMSPI_BlockedThreadInfo *threads,
				    int count);
void fplinux_pm_network_cancel(void);
void fplinux_pm_network_cleanup(void);

#endif
