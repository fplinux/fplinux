/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_QUICK_SETTINGS_H
#define FPLINUX_LAUNCHER_QUICK_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "fplinux-ui-screen.h"
#include "fplinux-ui-session.h"

struct fplinux_bluez_adapter;
struct fplinux_ui;

enum fplinux_launcher_quick_setting {
	FPLINUX_LAUNCHER_QUICK_SETTING_BRIGHTNESS,
	FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME,
	FPLINUX_LAUNCHER_QUICK_SETTING_WIFI,
	FPLINUX_LAUNCHER_QUICK_SETTING_BLUETOOTH,
	FPLINUX_LAUNCHER_QUICK_SETTING_COUNT,
};

struct fplinux_launcher_quick_settings_listener {
	void (*view_changed)(void *context);
	void *context;
};

struct fplinux_launcher_quick_level {
	int value;
	bool known;
	bool unsupported;
};

struct fplinux_launcher_quick_settings {
	struct fplinux_ui *ui;
	struct fplinux_bluez_adapter *bluetooth;
	struct fplinux_launcher_quick_settings_listener listener;
	struct fplinux_ui_screen *list;
	struct fplinux_ui_screen *alert;
	enum fplinux_launcher_quick_setting
		rows[FPLINUX_LAUNCHER_QUICK_SETTING_COUNT];
	size_t row_count;
	struct fplinux_launcher_quick_level levels[2];
	enum fplinux_launcher_quick_setting editing_row;
	int draft;
	int preview_sent;
	bool editing;
	bool preview_pending;
	bool save_requested;
	bool save_pending;
	bool wifi_known;
	bool wifi_on;
	bool wifi_pending;
	bool wifi_turning_on;
	bool bluetooth_turning_on;
	bool bluetooth_owned;
	bool blocked;
};

void fplinux_launcher_quick_settings_init(
	struct fplinux_launcher_quick_settings *quick, struct fplinux_ui *ui,
	struct fplinux_bluez_adapter *bluetooth,
	const struct fplinux_launcher_quick_settings_listener *listener);
bool fplinux_launcher_quick_settings_is_open(
	const struct fplinux_launcher_quick_settings *quick);
void fplinux_launcher_quick_settings_open(
	struct fplinux_launcher_quick_settings *quick);
void fplinux_launcher_quick_settings_close(
	struct fplinux_launcher_quick_settings *quick);
void fplinux_launcher_quick_settings_set_blocked(
	struct fplinux_launcher_quick_settings *quick, bool blocked);
bool fplinux_launcher_quick_settings_receive(
	struct fplinux_launcher_quick_settings *quick,
	const struct fplinux_ui_session_message *message);
void fplinux_launcher_quick_settings_bluetooth_changed(
	struct fplinux_launcher_quick_settings *quick);
void fplinux_launcher_quick_settings_bluetooth_finished(
	struct fplinux_launcher_quick_settings *quick, bool applied);

#endif
