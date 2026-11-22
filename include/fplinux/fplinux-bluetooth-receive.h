/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_BLUETOOTH_RECEIVE_H
#define FPLINUX_BLUETOOTH_RECEIVE_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef FPLINUX_BLUETOOTH_RECEIVE_SOCKET
#define FPLINUX_BLUETOOTH_RECEIVE_SOCKET "/run/fplinux-obex-receive.sock"
#endif

#define FPLINUX_BLUETOOTH_RECEIVE_CONTROL_FD 3
#define FPLINUX_BLUETOOTH_RECEIVE_LISTENER_FD 4
#define FPLINUX_BLUETOOTH_RECEIVE_WORKER "--session-receive"
#define FPLINUX_BLUETOOTH_RECEIVE_PATH_BYTES 4096

enum fplinux_session_bluetooth_state {
	FPLINUX_SESSION_BLUETOOTH_READY,
	FPLINUX_SESSION_BLUETOOTH_OFF,
	FPLINUX_SESSION_BLUETOOTH_UNAVAILABLE,
	FPLINUX_SESSION_BLUETOOTH_BUSY,
	FPLINUX_SESSION_BLUETOOTH_OFFER,
	FPLINUX_SESSION_BLUETOOTH_TRANSFERRING,
	FPLINUX_SESSION_BLUETOOTH_COMPLETE,
	FPLINUX_SESSION_BLUETOOTH_FAILED,
	FPLINUX_SESSION_BLUETOOTH_CLEARED,
};

struct fplinux_session_bluetooth_event {
	enum fplinux_session_bluetooth_state state;
	uint32_t id;
	uint64_t transferred_bytes;
	uint64_t total_bytes;
	bool total_known;
	char peer[256];
	char filename[256];
	char path[FPLINUX_BLUETOOTH_RECEIVE_PATH_BYTES];
};

struct fplinux_bluetooth_receive_decision {
	uint32_t id;
	bool accept;
};

/* A successful claim may return -1 in headless operation. Close a returned
 * descriptor only after the CLI's registered receive agent has stopped. */
int fplinux_bluetooth_receive_claim(int *lease,
				    const volatile sig_atomic_t *interrupted);
int fplinux_bluetooth_receive_worker(const char *home,
				     const volatile sig_atomic_t *interrupted);

#endif
