/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SETTINGS_BLUETOOTH_H
#define FPLINUX_SETTINGS_BLUETOOTH_H

#include <stdbool.h>

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-storage.h>

struct fplinux_bluez_adapter;
struct fplinux_settings_bluetooth;

/* Borrows ui and the application's one adapter; neither is closed here. */
struct fplinux_settings_bluetooth *
fplinux_settings_bluetooth_new(struct fplinux_ui *ui,
			       struct fplinux_bluez_adapter *adapter);
/* Before fplinux_ui_close(): cancels and joins its transfer worker. */
void fplinux_settings_bluetooth_free(
	struct fplinux_settings_bluetooth *bluetooth);
void fplinux_settings_bluetooth_show(
	struct fplinux_settings_bluetooth *bluetooth);
bool fplinux_settings_bluetooth_shown(
	const struct fplinux_settings_bluetooth *bluetooth);
void fplinux_settings_bluetooth_changed(
	struct fplinux_settings_bluetooth *bluetooth);
void fplinux_settings_bluetooth_finished(
	struct fplinux_settings_bluetooth *bluetooth, bool applied);
void fplinux_settings_bluetooth_event(
	struct fplinux_settings_bluetooth *bluetooth,
	const struct fplinux_ui_event *event);
void fplinux_settings_bluetooth_storage_result(
	struct fplinux_settings_bluetooth *bluetooth,
	const struct fplinux_volume_result *result);
/* Removes its screen subtree before Settings rebuilds its root. Transfers
 * continue; discovery and pairing prompts are released when hidden. */
void fplinux_settings_bluetooth_reset(
	struct fplinux_settings_bluetooth *bluetooth);
/* A running transfer asks before cancellation; WAIT is answered internally
 * with fplinux_ui_close_reply() when its worker has stopped. */
enum fplinux_ui_close_response fplinux_settings_bluetooth_prepare_close(
	struct fplinux_settings_bluetooth *bluetooth);

#endif
