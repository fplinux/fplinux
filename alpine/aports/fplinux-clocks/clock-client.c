/* SPDX-License-Identifier: GPL-2.0-only */
#include "clock-client.h"

#include <stdio.h>
#include <string.h>

static bool send_request(struct fplinux_ui *ui,
			 const struct clock_request *request, uint32_t serial)
{
	struct fplinux_ui_session_message message = { 0 };
	const char *name = clock_wire_request_name(request->op);

	if (!name ||
	    clock_wire_request_encode(request, message.clock_data,
				      sizeof(message.clock_data)) != CLOCK_OK)
		return false;
	message.type = FPLINUX_UI_SESSION_CLOCK;
	message.serial = serial;
	snprintf(message.name, sizeof(message.name), "%s", name);
	if (!fplinux_ui_send(ui, &message))
		return false;
	return true;
}

bool clock_client_send(struct clock_client *client, struct fplinux_ui *ui,
		       const struct clock_request *request, uint64_t now_ms)
{
	if (!client || !ui || !request ||
	    (client->pending && client->request.op != CLOCK_REQUEST_SAVE &&
	     (client->request.op != CLOCK_REQUEST_SNAPSHOT ||
	      request->op == CLOCK_REQUEST_SNAPSHOT)))
		return false;
	if (++client->serial == 0)
		++client->serial;
	if (!send_request(ui, request, client->serial))
		return false;
	client->request = *request;
	client->pending = true;
	client->deadline_ms = now_ms + 3000;
	return true;
}

bool clock_client_retry(struct clock_client *client, struct fplinux_ui *ui,
			uint64_t now_ms)
{
	if (!client || !ui || !client->pending ||
	    client->request.op == CLOCK_REQUEST_SNAPSHOT)
		return false;
	client->deadline_ms = now_ms + 3000;
	return send_request(ui, &client->request, client->serial);
}

enum clock_client_result
clock_client_receive(struct clock_client *client,
		     const struct fplinux_ui_session_message *message,
		     enum clock_status *status)
{
	struct clock_model snapshot;
	struct clock_result decoded_result;
	uint64_t serial;
	const char *name;
	size_t length;

	if (!client || !message || !status || !client->pending ||
	    message->serial != client->serial)
		return CLOCK_CLIENT_IGNORED;
	if (message->type == FPLINUX_UI_SESSION_ERROR) {
		client->pending = client->request.op != CLOCK_REQUEST_SNAPSHOT;
		client->deadline_ms = 0;
		return CLOCK_CLIENT_ERROR;
	}
	name = clock_wire_request_name(client->request.op);
	if (message->type != FPLINUX_UI_SESSION_CLOCK || !name ||
	    strcmp(message->name, name))
		return CLOCK_CLIENT_IGNORED;
	for (length = 0; length < sizeof(message->clock_data); ++length)
		if (!message->clock_data[length])
			break;
	if (length == sizeof(message->clock_data) ||
	    clock_wire_snapshot_decode(message->clock_data, length, &snapshot,
				       &decoded_result, &serial) != CLOCK_OK ||
	    serial != message->serial) {
		client->pending = client->request.op != CLOCK_REQUEST_SNAPSHOT;
		client->deadline_ms = 0;
		return CLOCK_CLIENT_ERROR;
	}
	client->snapshot = snapshot;
	client->result = decoded_result;
	client->received = true;
	client->pending = false;
	client->deadline_ms = 0;
	*status = decoded_result.status;
	return CLOCK_CLIENT_SNAPSHOT;
}

bool clock_client_expire(struct clock_client *client, uint64_t now_ms)
{
	if (!client || !client->pending || now_ms < client->deadline_ms)
		return false;
	client->pending = false;
	return true;
}
