/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_WIFI_DHCP_H
#define FPLINUX_SESSION_WIFI_DHCP_H

#include <arpa/inet.h>

struct fplinux_session_wifi_dhcp;

enum fplinux_session_wifi_dhcp_state {
	FPLINUX_SESSION_WIFI_DHCP_WAITING,
	FPLINUX_SESSION_WIFI_DHCP_BOUND,
	FPLINUX_SESSION_WIFI_DHCP_FAILED,
};

struct fplinux_session_wifi_dhcp *
fplinux_session_wifi_dhcp_start(const char *interface);
enum fplinux_session_wifi_dhcp_state
fplinux_session_wifi_dhcp_read(struct fplinux_session_wifi_dhcp *dhcp,
			       char address[INET_ADDRSTRLEN]);
void fplinux_session_wifi_dhcp_destroy(struct fplinux_session_wifi_dhcp *dhcp);

#endif
