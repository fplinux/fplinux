/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_WIFI_STATION_H
#define FPLINUX_SESSION_WIFI_STATION_H

#include "fplinux-ui-session.h"

struct fplinux_session_wifi_station;

/* Call from the wireless worker after it has selected the WLAN. */
struct fplinux_session_wifi_station *
fplinux_session_wifi_station_create(const char *ifname,
				    const char *config_path);
void fplinux_session_wifi_station_destroy(
	struct fplinux_session_wifi_station *station);

enum fplinux_ui_wifi_result fplinux_session_wifi_station_acquire(
	struct fplinux_session_wifi_station *station);

/* A true return supplies an immediate result. Otherwise use take_result. */
bool fplinux_session_wifi_station_request(
	struct fplinux_session_wifi_station *station,
	const struct fplinux_ui_wifi_request *request, uint32_t serial,
	enum fplinux_ui_wifi_result *result);
void fplinux_session_wifi_station_update(
	struct fplinux_session_wifi_station *station);
bool fplinux_session_wifi_station_take_result(
	struct fplinux_session_wifi_station *station, uint32_t *serial,
	enum fplinux_ui_wifi_result *result);

/* Publish the selected profile once the caller finishes the address attempt. */
enum fplinux_ui_wifi_result fplinux_session_wifi_station_commit(
	struct fplinux_session_wifi_station *station);

/* ADDRESSING means the selected network is associated. The caller owns IPv4. */
const struct fplinux_ui_wifi_state *fplinux_session_wifi_station_state(
	const struct fplinux_session_wifi_station *station);

#endif
