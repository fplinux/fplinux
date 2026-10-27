/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_STORAGE_H
#define FPLINUX_UI_STORAGE_H

#include "fplinux-volumes.h"

struct fplinux_ui;

/* Result and strings are borrowed for the duration of the callback. */
typedef void (*fplinux_ui_storage_cb)(struct fplinux_ui *ui,
				      const struct fplinux_volume_result *result,
				      void *context);

void fplinux_ui_set_storage_handler(struct fplinux_ui *ui,
				    fplinux_ui_storage_cb callback,
				    void *context);
/*
 * Returns a request ID, or zero with errno when it could not be sent.
 * REFRESH takes volume_id zero. CONNECT/EJECT use an ID from the current
 * snapshot, never a caller-supplied device or mount path. The result arrives
 * asynchronously; sending is not evidence of a successful mount or unmount.
 */
uint32_t fplinux_ui_storage_request(struct fplinux_ui *ui,
				    enum fplinux_volume_operation operation,
				    uint32_t volume_id);

#endif
