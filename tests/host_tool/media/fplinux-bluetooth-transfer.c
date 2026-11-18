// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fplinux-bluetooth-opp.h"

struct callbacks_state {
	const char *policy;
	bool cancel;
	int64_t cancel_at_ms;
};

static int64_t now_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool cancelled(void *data)
{
	struct callbacks_state *state = data;
	return state->cancel ||
	       (state->cancel_at_ms != 0 && now_ms() >= state->cancel_at_ms);
}

static void progress(const struct fplinux_bluetooth_opp_progress *progress,
		     void *data)
{
	struct callbacks_state *state = data;
	printf("progress %d %s %llu %llu %d\n", progress->state,
	       progress->filename,
	       (unsigned long long)progress->transferred_bytes,
	       (unsigned long long)progress->total_bytes,
	       progress->total_known);
	if (strcmp(state->policy, "cancel-progress") == 0 &&
	    progress->transferred_bytes >= 3)
		state->cancel = true;
}

static enum fplinux_bluetooth_opp_consent
authorize(const struct fplinux_bluetooth_opp_offer *offer, void *data)
{
	struct callbacks_state *state = data;
	printf("offer %s %s %llu %d\n", offer->peer, offer->filename,
	       (unsigned long long)offer->total_bytes, offer->total_known);
	if (strcmp(state->policy, "cancel-offer") == 0)
		state->cancel = true;
	if (strcmp(state->policy, "pending") == 0)
		return FPLINUX_BLUETOOTH_OPP_PENDING;
	return strcmp(state->policy, "reject") == 0 ?
		       FPLINUX_BLUETOOTH_OPP_REJECT :
		       FPLINUX_BLUETOOTH_OPP_ACCEPT;
}

static enum fplinux_bluetooth_opp_conflict conflict(const char *filename,
						    void *data)
{
	struct callbacks_state *state = data;
	printf("conflict %s\n", filename);
	if (strcmp(state->policy, "keep-both") == 0)
		return FPLINUX_BLUETOOTH_OPP_KEEP_BOTH;
	if (strcmp(state->policy, "replace") == 0)
		return FPLINUX_BLUETOOTH_OPP_REPLACE;
	return FPLINUX_BLUETOOTH_OPP_KEEP_EXISTING;
}

int main(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	struct callbacks_state state = { .policy = argv[3] };
	if (strcmp(state.policy, "cancel-wait") == 0)
		state.cancel_at_ms = now_ms() + 200;
	struct fplinux_bluetooth_opp_callbacks callbacks = {
		.cancelled = cancelled,
		.progress = progress,
		.authorize = authorize,
		.conflict = conflict,
		.data = &state,
	};
	struct fplinux_bluetooth_opp_result result;
	int status;
	if (strcmp(argv[1], "send") == 0)
		status = fplinux_bluetooth_opp_send(
			"01:23:45:67:89:AB", argv[2], &callbacks, &result);
	else
		status = fplinux_bluetooth_opp_receive(
			"01:23:45:67:89:AB", argv[2], 1, &callbacks, &result);
	printf("result %d %d %d %s\n", result.outcome, result.error_number,
	       result.published, result.path);
	printf("message %s\n", result.message);
	return status;
}
