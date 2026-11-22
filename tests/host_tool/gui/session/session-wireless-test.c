/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-wifi-dhcp.h"
#include "session-wifi-station.h"
#include "session-wireless.h"

#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The fake owns the Linux network boundary; the worker and its pipes are real. */
static struct {
	atomic_int interfaces;
	atomic_int wireless;
	atomic_int up;
	atomic_int set_error;
	atomic_int get_error;
	atomic_int ignore_set;
	atomic_int hold_get;
	atomic_int entered_get;
	atomic_int hold_set;
	atomic_int entered_set;
} network;

static int events[2];
static atomic_int listener_fd;

struct fplinux_session_wifi_station {
	struct fplinux_ui_wifi_state state;
	uint32_t ticket;
};

struct fplinux_session_wifi_dhcp {
	bool active;
};

static struct fplinux_session_wifi_station station;
static struct fplinux_session_wifi_dhcp dhcp;

int __real_access(const char *path, int mode);
int __real_socket(int domain, int type, int protocol);
int __real_bind(int fd, const struct sockaddr *address, socklen_t length);

static void require(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

int __wrap_access(const char *path, int mode)
{
	if (!strcmp(path, "/sbin/wpa_supplicant"))
		return 0;
	return __real_access(path, mode);
}

struct fplinux_session_wifi_station *
fplinux_session_wifi_station_create(const char *ifname, const char *config_path)
{
	(void)config_path;
	require(!strcmp(ifname, "test0"),
		"station receives the chosen interface");
	return &station;
}

void fplinux_session_wifi_station_destroy(
	struct fplinux_session_wifi_station *peer)
{
	if (peer)
		*peer = (struct fplinux_session_wifi_station){ 0 };
}

enum fplinux_ui_wifi_result
fplinux_session_wifi_station_acquire(struct fplinux_session_wifi_station *peer)
{
	(void)peer;
	return FPLINUX_UI_WIFI_APPLIED;
}

bool fplinux_session_wifi_station_request(
	struct fplinux_session_wifi_station *peer,
	const struct fplinux_ui_wifi_request *request, uint32_t serial,
	enum fplinux_ui_wifi_result *result)
{
	(void)result;
	peer->ticket = serial;
	if (request->action == FPLINUX_UI_WIFI_SCAN) {
		peer->state.scan = FPLINUX_UI_WIFI_SCAN_READY;
	} else {
		require(request->action == FPLINUX_UI_WIFI_CONNECT_HIDDEN,
			"station receives the manual join request");
		peer->state.link = FPLINUX_UI_WIFI_ADDRESSING;
		peer->state.current = (struct fplinux_ui_wifi_network){
			.id = 42,
			.security = FPLINUX_UI_WIFI_WPA2,
			.name = "Test AP",
		};
	}
	return false;
}

void fplinux_session_wifi_station_update(
	struct fplinux_session_wifi_station *peer)
{
	(void)peer;
}

bool fplinux_session_wifi_station_take_result(
	struct fplinux_session_wifi_station *peer, uint32_t *serial,
	enum fplinux_ui_wifi_result *result)
{
	if (!peer->ticket)
		return false;
	*serial = peer->ticket;
	*result = FPLINUX_UI_WIFI_APPLIED;
	peer->ticket = 0;
	return true;
}

enum fplinux_ui_wifi_result
fplinux_session_wifi_station_commit(struct fplinux_session_wifi_station *peer)
{
	(void)peer;
	return FPLINUX_UI_WIFI_APPLIED;
}

const struct fplinux_ui_wifi_state *fplinux_session_wifi_station_state(
	const struct fplinux_session_wifi_station *peer)
{
	return &peer->state;
}

struct fplinux_session_wifi_dhcp *
fplinux_session_wifi_dhcp_start(const char *interface)
{
	(void)interface;
	dhcp.active = true;
	return &dhcp;
}

enum fplinux_session_wifi_dhcp_state
fplinux_session_wifi_dhcp_read(struct fplinux_session_wifi_dhcp *lease,
			       char address[INET_ADDRSTRLEN])
{
	require(lease->active, "the DHCP lease is active");
	strcpy(address, "192.0.2.24");
	return FPLINUX_SESSION_WIFI_DHCP_BOUND;
}

void fplinux_session_wifi_dhcp_destroy(struct fplinux_session_wifi_dhcp *lease)
{
	if (lease)
		lease->active = false;
}

int __wrap_socket(int domain, int type, int protocol)
{
	int fd;

	if (domain != AF_NETLINK)
		return __real_socket(domain, type, protocol);
	fd = dup(events[0]);
	atomic_store(&listener_fd, fd);
	return fd;
}

int __wrap_bind(int fd, const struct sockaddr *address, socklen_t length)
{
	if (fd == atomic_load(&listener_fd))
		return 0;
	return __real_bind(fd, address, length);
}

struct if_nameindex *__wrap_if_nameindex(void)
{
	struct if_nameindex *interfaces = calloc(3, sizeof(*interfaces));
	int count = atomic_load(&network.interfaces);

	if (!interfaces)
		return NULL;
	if (count > 0) {
		interfaces[0].if_index = 7;
		interfaces[0].if_name = strdup("test0");
	}
	if (count > 1) {
		interfaces[1].if_index = 8;
		interfaces[1].if_name = strdup("test1");
	}
	return interfaces;
}

void __wrap_if_freenameindex(struct if_nameindex *interfaces)
{
	for (struct if_nameindex *entry = interfaces; entry->if_index; ++entry)
		free(entry->if_name);
	free(interfaces);
}

int __wrap_stat(const char *path, struct stat *status)
{
	(void)status;
	if (atomic_load(&network.wireless) &&
	    (!strcmp(path, "/sys/class/net/test0/phy80211") ||
	     !strcmp(path, "/sys/class/net/test1/phy80211")))
		return 0;
	errno = ENOENT;
	return -1;
}

int __wrap_ioctl(int fd, unsigned long command, ...)
{
	struct timespec delay = { .tv_nsec = 1000000 };
	struct ifreq *request;
	va_list arguments;
	int error;

	(void)fd;
	va_start(arguments, command);
	request = va_arg(arguments, struct ifreq *);
	va_end(arguments);
	if (command == SIOCGIFFLAGS) {
		if (atomic_load(&network.hold_get)) {
			atomic_store(&network.entered_get, 1);
			while (atomic_load(&network.hold_get))
				nanosleep(&delay, NULL);
		}
		error = atomic_load(&network.get_error);
		if (error) {
			errno = error;
			return -1;
		}
		request->ifr_flags = atomic_load(&network.up) ? IFF_UP : 0;
		return 0;
	}
	if (command != SIOCSIFFLAGS) {
		errno = ENOTTY;
		return -1;
	}
	atomic_store(&network.entered_set, 1);
	while (atomic_load(&network.hold_set))
		nanosleep(&delay, NULL);
	error = atomic_load(&network.set_error);
	if (error) {
		errno = error;
		return -1;
	}
	if (!atomic_load(&network.ignore_set))
		atomic_store(&network.up, (request->ifr_flags & IFF_UP) != 0);
	return 0;
}

static void wait_notification(struct fplinux_session_wireless *wireless)
{
	struct pollfd fd = { .fd = fplinux_session_wireless_fd(wireless),
			     .events = POLLIN };
	int result;

	do {
		result = poll(&fd, 1, 3000);
	} while (result < 0 && errno == EINTR);
	require(result == 1 && (fd.revents & POLLIN), "worker notification");
	fplinux_session_wireless_dispatch(wireless);
}

static int finish(struct fplinux_session_wireless *wireless)
{
	int result;

	for (;;) {
		wait_notification(wireless);
		if (fplinux_session_wireless_take_result(wireless, &result))
			return result;
	}
}

static bool state_matches(struct fplinux_session_wireless *wireless,
			  bool supported, bool known, bool enabled, int error)
{
	struct fplinux_session_wireless_state state =
		fplinux_session_wireless_state(wireless);

	return state.supported == supported && state.known == known &&
	       state.enabled == enabled && state.error == error;
}

static void expect_state(struct fplinux_session_wireless *wireless,
			 bool supported, bool known, bool enabled, int error)
{
	require(state_matches(wireless, supported, known, enabled, error),
		"wireless snapshot reflects actual interface state");
}

static void signal_change(void)
{
	char byte = 1;

	require(write(events[1], &byte, sizeof(byte)) == (ssize_t)sizeof(byte),
		"send fake link notification");
}

static void expect_change(struct fplinux_session_wireless *wireless,
			  bool supported, bool known, bool enabled, int error)
{
	signal_change();
	for (int attempt = 0; attempt < 4; ++attempt) {
		wait_notification(wireless);
		if (state_matches(wireless, supported, known, enabled, error))
			return;
	}
	require(false, "link notification updates actual state");
}

static void wait_for_ioctl(atomic_int *entered, const char *message)
{
	struct timespec delay = { .tv_nsec = 1000000 };

	for (int attempt = 0; attempt < 1000 && !atomic_load(entered);
	     ++attempt)
		nanosleep(&delay, NULL);
	require(atomic_load(entered), message);
}

static void cancel_queued_connection(struct fplinux_session_wireless *wireless)
{
	const struct fplinux_ui_wifi_request connect = {
		.action = FPLINUX_UI_WIFI_CONNECT_HIDDEN,
		.security = FPLINUX_UI_WIFI_WPA2,
		.save = true,
		.ssid = "test-network",
		.key = "test-password",
	};
	struct fplinux_ui_wifi_request cancel = {
		.action = FPLINUX_UI_WIFI_CANCEL,
		.target = 99,
	};
	enum fplinux_ui_wifi_result result;
	struct fplinux_ui_wifi_network connected;
	uint32_t ticket;

	atomic_store(&network.entered_get, 0);
	atomic_store(&network.hold_get, 1);
	signal_change();
	wait_for_ioctl(&network.entered_get,
		       "hold worker before reading Wi-Fi requests");
	require(fplinux_session_wireless_wifi_request(wireless, &connect,
						      100) == 0,
		"connection request is queued");
	require(fplinux_session_wireless_wifi_request(wireless, &cancel, 101) ==
			EBUSY,
		"a different ticket does not cancel the queued connection");
	cancel.target = 100;
	require(fplinux_session_wireless_wifi_request(wireless, &cancel, 102) ==
			0,
		"cancel is accepted before the worker dequeues the connection");
	wait_notification(wireless);
	require(fplinux_session_wireless_take_wifi_result(
			wireless, &ticket, &result, &connected) &&
			ticket == 102 && result == FPLINUX_UI_WIFI_APPLIED &&
			!connected.id,
		"result acknowledges cancel while the worker is still held");
	require(fplinux_session_wireless_request(wireless, false) == 0,
		"cancelled connection no longer occupies the request queue");
	atomic_store(&network.hold_get, 0);
	require(finish(wireless) == 0, "worker continues after cancellation");
	require(!fplinux_session_wireless_take_wifi_result(wireless, &ticket,
							   &result, &connected),
		"cancelled connection does not produce a second result");
}

static void scan_after_join(struct fplinux_session_wireless *wireless)
{
	const struct fplinux_ui_wifi_request connect = {
		.action = FPLINUX_UI_WIFI_CONNECT_HIDDEN,
		.security = FPLINUX_UI_WIFI_WPA2,
		.ssid = "Test AP",
		.key = "test-password",
	};
	const struct fplinux_ui_wifi_request scan = {
		.action = FPLINUX_UI_WIFI_SCAN
	};
	struct fplinux_ui_wifi_network connected;
	struct fplinux_ui_wifi_state state;
	enum fplinux_ui_wifi_result result;
	uint32_t ticket;

	require(fplinux_session_wireless_wifi_request(wireless, &connect,
						      201) == 0,
		"join is queued");
	wait_notification(wireless);
	require(fplinux_session_wireless_take_wifi_result(
			wireless, &ticket, &result, &connected) &&
			ticket == 201 && result == FPLINUX_UI_WIFI_APPLIED &&
			!strcmp(connected.name, "Test AP"),
		"the address result identifies the chosen network");
	require(fplinux_session_wireless_wifi_request(wireless, &scan, 202) ==
			0,
		"scan is queued while associated");
	wait_notification(wireless);
	require(fplinux_session_wireless_take_wifi_result(
			wireless, &ticket, &result, &connected) &&
			ticket == 202 && result == FPLINUX_UI_WIFI_APPLIED &&
			!connected.id,
		"a completed scan has no connection to announce");
	fplinux_session_wireless_wifi_state(wireless, &state);
	require(state.link == FPLINUX_UI_WIFI_CONNECTED &&
			!strcmp(state.address, "192.0.2.24"),
		"refresh retains the existing connection");
}

int main(void)
{
	struct fplinux_session_wireless *wireless;
	struct timespec start, end;
	int notification_fd;

	require(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
			   0, events) == 0,
		"fake link notification socket");
	wireless = fplinux_session_wireless_create();
	require(wireless != NULL, "create wireless backend");
	notification_fd = fplinux_session_wireless_fd(wireless);
	wait_notification(wireless);
	expect_state(wireless, false, false, false, 0);
	require(fplinux_session_wireless_request(wireless, true) == 0,
		"request accepted when interface absent");
	require(finish(wireless) == ENODEV, "absent interface reports ENODEV");

	atomic_store(&network.interfaces, 1);
	expect_change(wireless, false, false, false, 0);
	atomic_store(&network.wireless, 1);
	expect_change(wireless, true, true, false, 0);
	cancel_queued_connection(wireless);
	require(fplinux_session_wireless_request(wireless, true) == 0,
		"enable requested");
	require(finish(wireless) == 0, "enable confirmed");
	expect_state(wireless, true, true, true, 0);
	scan_after_join(wireless);
	atomic_store(&network.up, 0);
	expect_change(wireless, true, true, false, 0);
	atomic_store(&network.up, 1);
	require(fplinux_session_wireless_request(wireless, false) == 0,
		"disable requested");
	require(finish(wireless) == 0, "disable confirmed");
	expect_state(wireless, true, true, false, 0);

	atomic_store(&network.set_error, EPERM);
	require(fplinux_session_wireless_request(wireless, true) == 0,
		"failing request accepted");
	require(finish(wireless) == EPERM, "ioctl errno preserved");
	expect_state(wireless, true, true, false, 0);
	atomic_store(&network.set_error, 0);
	atomic_store(&network.ignore_set, 1);
	require(fplinux_session_wireless_request(wireless, true) == 0,
		"unconfirmed request accepted");
	require(finish(wireless) == EIO,
		"success requires matching actual state");
	atomic_store(&network.ignore_set, 0);
	atomic_store(&network.get_error, EIO);
	expect_change(wireless, true, false, false, EIO);
	atomic_store(&network.get_error, 0);
	atomic_store(&network.interfaces, 2);
	expect_change(wireless, false, false, false, EOPNOTSUPP);
	atomic_store(&network.interfaces, 1);
	atomic_store(&network.hold_get, 1);
	signal_change();
	wait_for_ioctl(&network.entered_get, "worker entered slow state read");
	clock_gettime(CLOCK_MONOTONIC, &start);
	(void)fplinux_session_wireless_state(wireless);
	fplinux_session_wireless_dispatch(wireless);
	require(fplinux_session_wireless_request(wireless, true) == 0,
		"request accepted while discovery waits on kernel");
	clock_gettime(CLOCK_MONOTONIC, &end);
	require((end.tv_sec - start.tv_sec) * 1000000000LL + end.tv_nsec -
				start.tv_nsec <
			200000000LL,
		"main thread does not wait for state discovery");
	atomic_store(&network.hold_get, 0);
	require(finish(wireless) == 0,
		"request after slow discovery confirmed");

	atomic_store(&network.entered_set, 0);
	atomic_store(&network.hold_set, 1);
	require(fplinux_session_wireless_request(wireless, true) == 0,
		"slow operation accepted");
	wait_for_ioctl(&network.entered_set, "worker entered slow change");
	clock_gettime(CLOCK_MONOTONIC, &start);
	fplinux_session_wireless_dispatch(wireless);
	(void)fplinux_session_wireless_state(wireless);
	clock_gettime(CLOCK_MONOTONIC, &end);
	require((end.tv_sec - start.tv_sec) * 1000000000LL + end.tv_nsec -
				start.tv_nsec <
			200000000LL,
		"main thread does not wait for the ioctl");
	require(fplinux_session_wireless_request(wireless, false) == EBUSY,
		"another request cannot overtake the pending operation");
	atomic_store(&network.hold_set, 0);
	require(finish(wireless) == 0, "slow operation confirmed");
	require(fplinux_session_wireless_fd(wireless) == notification_fd,
		"notification descriptor remains stable");

	atomic_store(&network.entered_set, 0);
	atomic_store(&network.hold_set, 1);
	require(fplinux_session_wireless_request(wireless, false) == 0,
		"request accepted before removal");
	wait_for_ioctl(&network.entered_set,
		       "worker entered change before removal");
	atomic_store(&network.interfaces, 0);
	signal_change();
	atomic_store(&network.hold_set, 0);
	require(finish(wireless) == ENODEV,
		"removed interface cannot confirm a request");
	expect_state(wireless, false, false, false, 0);
	fplinux_session_wireless_destroy(wireless);
	close(events[0]);
	close(events[1]);
	puts("session wireless: OK");
	return 0;
}
