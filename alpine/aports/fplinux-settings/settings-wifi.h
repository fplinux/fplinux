/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SETTINGS_WIFI_H
#define FPLINUX_SETTINGS_WIFI_H

#include "fplinux-ui-session.h"

struct fplinux_settings_wifi;

struct fplinux_settings_wifi *fplinux_settings_wifi_new(struct fplinux_ui *ui);
void fplinux_settings_wifi_free(struct fplinux_settings_wifi *wifi);
void fplinux_settings_wifi_show(struct fplinux_settings_wifi *wifi);
void fplinux_settings_wifi_reset(struct fplinux_settings_wifi *wifi);
void fplinux_settings_wifi_background(struct fplinux_settings_wifi *wifi);
void fplinux_settings_wifi_foreground(struct fplinux_settings_wifi *wifi);
bool fplinux_settings_wifi_shown(const struct fplinux_settings_wifi *wifi);
enum fplinux_ui_wifi_power
fplinux_settings_wifi_power(const struct fplinux_settings_wifi *wifi);
void fplinux_settings_wifi_receive(
	struct fplinux_settings_wifi *wifi,
	const struct fplinux_ui_session_message *message);

#endif
