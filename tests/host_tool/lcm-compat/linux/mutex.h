/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LCM_HOST_MUTEX_H
#define LCM_HOST_MUTEX_H
/* Layout placeholder only: the transport never locks; no locking is tested. */
struct mutex {
	unsigned int unused;
};
#endif
