/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CLOCK_CLIENT_H
#define FPLINUX_CLOCK_CLIENT_H

#include "clock-wire.h"
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-session.h>

/* One outstanding request keeps actions ordered and bounds owner traffic. */
struct clock_client {
	struct clock_model snapshot;
	struct clock_request request;
	struct clock_result result;
	uint32_t serial;
	uint64_t deadline_ms;
	bool received;
	bool pending;
};

enum clock_client_result {
	CLOCK_CLIENT_IGNORED,
	CLOCK_CLIENT_SNAPSHOT,
	CLOCK_CLIENT_ERROR,
};

bool clock_client_send(struct clock_client *client, struct fplinux_ui *ui,
		       const struct clock_request *request, uint64_t now_ms);
/* Retransmits the same mutation and serial; the owner acknowledges duplicates. */
bool clock_client_retry(struct clock_client *client, struct fplinux_ui *ui,
			uint64_t now_ms);
enum clock_client_result
clock_client_receive(struct clock_client *client,
		     const struct fplinux_ui_session_message *message,
		     enum clock_status *status);
/* Called by the visible UI's bounded timer, never by a hidden animation. */
bool clock_client_expire(struct clock_client *client, uint64_t now_ms);

#endif
