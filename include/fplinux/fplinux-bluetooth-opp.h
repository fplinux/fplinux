// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_BLUETOOTH_OPP_H
#define FPLINUX_BLUETOOTH_OPP_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum fplinux_bluetooth_opp_state {
	FPLINUX_BLUETOOTH_OPP_CONNECTING,
	FPLINUX_BLUETOOTH_OPP_WAITING,
	FPLINUX_BLUETOOTH_OPP_AUTHORIZING,
	FPLINUX_BLUETOOTH_OPP_TRANSFERRING,
	FPLINUX_BLUETOOTH_OPP_PUBLISHING,
	FPLINUX_BLUETOOTH_OPP_COMPLETE,
};

struct fplinux_bluetooth_opp_progress {
	enum fplinux_bluetooth_opp_state state;
	const char *peer;
	const char *filename;
	uint64_t transferred_bytes;
	uint64_t total_bytes;
	bool total_known;
};

struct fplinux_bluetooth_opp_offer {
	const char *peer;
	const char *filename;
	uint64_t total_bytes;
	bool total_known;
};

enum fplinux_bluetooth_opp_conflict {
	FPLINUX_BLUETOOTH_OPP_KEEP_EXISTING,
	FPLINUX_BLUETOOTH_OPP_KEEP_BOTH,
	FPLINUX_BLUETOOTH_OPP_REPLACE,
};

enum fplinux_bluetooth_opp_consent {
	FPLINUX_BLUETOOTH_OPP_PENDING,
	FPLINUX_BLUETOOTH_OPP_REJECT,
	FPLINUX_BLUETOOTH_OPP_ACCEPT,
};

struct fplinux_bluetooth_opp_callbacks {
	bool (*cancelled)(void *data);
	void (*progress)(const struct fplinux_bluetooth_opp_progress *progress,
			 void *data);
	enum fplinux_bluetooth_opp_consent (*authorize)(
		const struct fplinux_bluetooth_opp_offer *offer, void *data);
	enum fplinux_bluetooth_opp_conflict (*conflict)(const char *filename,
							void *data);
	void *data;
};

enum fplinux_bluetooth_opp_outcome {
	FPLINUX_BLUETOOTH_OPP_FAILED,
	FPLINUX_BLUETOOTH_OPP_CANCELLED,
	FPLINUX_BLUETOOTH_OPP_SUCCEEDED,
};

struct fplinux_bluetooth_opp_result {
	enum fplinux_bluetooth_opp_outcome outcome;
	int error_number;
	/* A published file may remain visible if its directory sync failed. */
	bool published;
	char path[PATH_MAX];
	char message[512];
};

/* Calls block and own a private system-bus connection. Callbacks run on the
 * calling thread; their string pointers are borrowed for that callback only.
 * Authorize must return promptly and explicitly accept an incoming file. A
 * PENDING response is polled at most 50 ms apart while D-Bus remains serviced.
 * Conflict defaults to KEEP_EXISTING; REPLACE requires the caller's explicit
 * confirmation. A conflict callback waiting for UI consent must itself observe
 * cancellation. Inputs, callbacks and data must remain valid until return.
 * No GUI calls are made.
 * Return zero only after completion (and, for receive, durable publication).
 * On publication failure path names a retained complete staging file when
 * available. cancelled is checked at most 50 ms apart during D-Bus waits. */
int fplinux_bluetooth_opp_send(
	const char *peer, const char *file,
	const struct fplinux_bluetooth_opp_callbacks *callbacks,
	struct fplinux_bluetooth_opp_result *result);
int fplinux_bluetooth_opp_receive(
	const char *peer, const char *directory, int seconds,
	const struct fplinux_bluetooth_opp_callbacks *callbacks,
	struct fplinux_bluetooth_opp_result *result);

struct fplinux_bluetooth_opp_listener {
	/* A suspension request relinquishes the agent when no file is pending. */
	bool (*suspended)(bool busy, void *data);
	/* At offer creation, name receives the paired device's display name.
	 * Later checks use NULL and must verify the same pairing and BlueZ owner. */
	bool (*peer_allowed)(const char *local, const char *remote, char *name,
			     size_t name_size, void *data);
	void (*registered)(bool ready, void *data);
	/* Called once for each displayed offer, including expiry and rejection. */
	void (*finished)(bool accepted,
			 const struct fplinux_bluetooth_opp_result *result,
			 void *data);
	/* Wait on the application's own descriptors and on bus_fd when >= 0. */
	void (*wait)(int bus_fd, int timeout_ms, void *data);
};

/* Runs a persistent receive agent until cancelled. Creates the directory on
 * the first acceptance and defaults to keeping both names on conflicts. */
void fplinux_bluetooth_opp_listen(
	const char *directory,
	const struct fplinux_bluetooth_opp_callbacks *callbacks,
	const struct fplinux_bluetooth_opp_listener *listener);

#endif
