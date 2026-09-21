/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_CM4_SETUP_H
#define UMS9117_CM4_SETUP_H

#include <linux/types.h>

struct device;

#define UMS9117_CM4_VERSION_SIZE 6U

/* Load all fitted records before CM4 reset release. */
int ums9117_cm4_setup_prepare(struct device *dev, const char *const names[3],
			      const u8 version[UMS9117_CM4_VERSION_SIZE]);
/* Single IRQ-enabled caller after mailbox_poll; caller owns the deadline. */
int ums9117_cm4_setup_poll(void);
/* Transfer an already-read partial H4 event to the runtime RX owner. */
const u8 *ums9117_cm4_setup_rx_prefix(size_t *bytes);

#endif
