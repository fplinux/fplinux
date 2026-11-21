/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-wireless.h"
#include "session-wifi-dhcp.h"
#include "session-wifi-station.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define WIFI_CONFIG "/var/lib/fplinux/wifi/wpa_supplicant.conf"
#define WIFI_ACTIVE_POLL_MS 250
#define WIFI_IDLE_POLL_MS 1000

struct wireless_interface {
	unsigned int index;
	char name[IFNAMSIZ];
};

struct wifi_request {
	uint32_t ticket;
	struct fplinux_ui_wifi_request action;
};

struct wifi_result {
	uint32_t ticket;
	enum fplinux_ui_wifi_result result;
};

struct wifi_worker {
	struct fplinux_session_wifi_station *station;
	unsigned int interface_index;
	struct fplinux_session_wifi_dhcp *dhcp;
	enum fplinux_session_wifi_dhcp_state dhcp_state;
	bool association;
	char address[16];
	uint32_t operation;
	bool connecting;
	bool waiting_for_address;
};

struct fplinux_session_wireless {
	pthread_t thread;
	pthread_mutex_t mutex;
	int command[2];
	int notification[2];
	bool stop;
	bool pending;
	bool requested;
	bool ready;
	int worker_error;
	int result_error;
	struct fplinux_session_wireless_state state;
	struct fplinux_ui_wifi_state wifi_state;
	struct wifi_request wifi_request;
	struct wifi_result wifi_result;
};

/* A zero index means absent; ambiguity and failures are explicit errors. */
static int find_interface(struct wireless_interface *selected)
{
	struct if_nameindex *interfaces, *entry;
	int error = 0;

	memset(selected, 0, sizeof(*selected));
	interfaces = if_nameindex();
	if (!interfaces)
		return errno;
	for (entry = interfaces; entry->if_index && entry->if_name; ++entry) {
		char path[sizeof("/sys/class/net//phy80211") + IFNAMSIZ];
		struct stat status;

		if (snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211",
			     entry->if_name) >= (int)sizeof(path)) {
			error = ENAMETOOLONG;
			break;
		}
		if (stat(path, &status) < 0) {
			if (errno == ENOENT || errno == ENODEV)
				continue;
			error = errno;
			break;
		}
		if (selected->index) {
			error = EOPNOTSUPP;
			break;
		}
		selected->index = entry->if_index;
		snprintf(selected->name, sizeof(selected->name), "%s",
			 entry->if_name);
	}
	if_freenameindex(interfaces);
	return error;
}

static int interface_flags(const struct wireless_interface *interface, bool set,
			   bool enabled, bool *actual)
{
	struct ifreq request = { 0 };
	int fd, error = 0;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return errno;
	snprintf(request.ifr_name, sizeof(request.ifr_name), "%s",
		 interface->name);
	if (ioctl(fd, SIOCGIFFLAGS, &request) < 0) {
		error = errno;
	} else if (set) {
		if (enabled)
			request.ifr_flags |= IFF_UP;
		else
			request.ifr_flags &= ~IFF_UP;
		if (ioctl(fd, SIOCSIFFLAGS, &request) < 0)
			error = errno;
	} else {
		*actual = (request.ifr_flags & IFF_UP) != 0;
	}
	close(fd);
	return error;
}

static void snapshot(struct fplinux_session_wireless_state *state,
		     struct wireless_interface *interface)
{
	memset(state, 0, sizeof(*state));
	state->error = find_interface(interface);
	if (!state->error && interface->index) {
		state->supported = true;
		state->error = interface_flags(interface, false, false,
					       &state->enabled);
		state->known = !state->error;
	}
}

static int operate(bool enabled, struct fplinux_session_wireless_state *state,
		   struct wireless_interface *observed)
{
	struct wireless_interface interface;
	int error = find_interface(&interface);

	if (!error && !interface.index)
		error = ENODEV;
	if (!error)
		error = interface_flags(&interface, true, enabled, NULL);
	snapshot(state, observed);
	if (error)
		return error;
	if (state->error)
		return state->error;
	if (!observed->index || observed->index != interface.index)
		return ENODEV;
	if (!state->known || state->enabled != enabled)
		return EIO;
	return 0;
}

static void wake(int fd)
{
	char byte = 1;
	ssize_t result;

	do {
		result = write(fd, &byte, sizeof(byte));
	} while (result < 0 && errno == EINTR);
}

static void drain(int fd)
{
	char buffer[128];
	ssize_t result;

	do {
		result = read(fd, buffer, sizeof(buffer));
	} while (result > 0 || (result < 0 && errno == EINTR));
}

static void publish(struct fplinux_session_wireless *wireless,
		    const struct fplinux_session_wireless_state *state,
		    const struct fplinux_ui_wifi_state *wifi_state,
		    struct wifi_result wifi_result, bool result_ready,
		    int error, bool force_notification)
{
	bool changed;

	pthread_mutex_lock(&wireless->mutex);
	changed =
		force_notification || result_ready || wifi_result.ticket ||
		wireless->state.supported != state->supported ||
		wireless->state.known != state->known ||
		wireless->state.enabled != state->enabled ||
		wireless->state.error != state->error ||
		memcmp(&wireless->wifi_state, wifi_state, sizeof(*wifi_state));
	wireless->state = *state;
	wireless->wifi_state = *wifi_state;
	if (wifi_result.ticket)
		wireless->wifi_result = wifi_result;
	if (result_ready) {
		wireless->pending = false;
		wireless->ready = true;
		wireless->result_error = error;
	}
	pthread_mutex_unlock(&wireless->mutex);
	if (changed)
		wake(wireless->notification[1]);
}

static int open_listener(void)
{
	struct sockaddr_nl address = { .nl_family = AF_NETLINK,
				       .nl_groups = RTMGRP_LINK };
	int fd, error;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
		    NETLINK_ROUTE);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
		return fd;
	error = errno;
	close(fd);
	errno = error;
	return -1;
}

static int drain_listener(int fd)
{
	char buffer[8192];
	ssize_t result;

	do {
		result = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
	} while (result > 0 || (result < 0 && errno == EINTR));
	if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
	    errno != ENOBUFS)
		return errno;
	return result == 0 ? EIO : 0;
}

static void stop_address(struct wifi_worker *wifi)
{
	fplinux_session_wifi_dhcp_destroy(wifi->dhcp);
	wifi->dhcp = NULL;
	wifi->association = false;
	wifi->address[0] = '\0';
	wifi->dhcp_state = FPLINUX_SESSION_WIFI_DHCP_WAITING;
}

static struct wifi_result complete_wifi(struct wifi_worker *wifi,
					enum fplinux_ui_wifi_result result)
{
	struct wifi_result reply = { .ticket = wifi->operation,
				     .result = result };

	wifi->operation = 0;
	wifi->connecting = false;
	wifi->waiting_for_address = false;
	return reply;
}

static struct wifi_result stop_wifi(struct wifi_worker *wifi)
{
	stop_address(wifi);
	fplinux_session_wifi_station_destroy(wifi->station);
	wifi->station = NULL;
	wifi->interface_index = 0;
	return complete_wifi(wifi, FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE);
}

static struct wifi_result refresh_wifi(
	struct wifi_worker *wifi, const struct wireless_interface *interface,
	const struct fplinux_session_wireless_state *state, bool has_supplicant)
{
	struct wifi_result result = { 0 };

	if (wifi->station &&
	    (!has_supplicant || !state->known || !state->enabled ||
	     wifi->interface_index != interface->index))
		result = stop_wifi(wifi);
	if (!wifi->station && has_supplicant && state->known &&
	    state->enabled) {
		wifi->station = fplinux_session_wifi_station_create(
			interface->name, WIFI_CONFIG);
		if (wifi->station) {
			wifi->interface_index = interface->index;
			fplinux_session_wifi_station_acquire(wifi->station);
		}
	}
	return result;
}

static struct wifi_result request_wifi(struct wifi_worker *wifi,
				       const struct wifi_request *request)
{
	struct wifi_result reply = {
		.ticket = request->ticket,
		.result = FPLINUX_UI_WIFI_FAILED,
	};
	struct fplinux_ui_wifi_request action = request->action;

	if (!wifi->station)
		goto clear;
	if (action.action == FPLINUX_UI_WIFI_CANCEL) {
		if (!wifi->operation || action.target != wifi->operation)
			goto clear;
		if (wifi->waiting_for_address)
			action.action = FPLINUX_UI_WIFI_DISCONNECT;
	} else if (wifi->operation) {
		reply.result = FPLINUX_UI_WIFI_BUSY;
		goto clear;
	}
	if (action.action == FPLINUX_UI_WIFI_RETRY_ADDRESS) {
		if (!wifi->association ||
		    wifi->dhcp_state != FPLINUX_SESSION_WIFI_DHCP_FAILED)
			goto clear;
		stop_address(wifi);
		wifi->operation = request->ticket;
		wifi->waiting_for_address = true;
		reply.ticket = 0;
		goto clear;
	}
	if (!fplinux_session_wifi_station_request(
		    wifi->station, &action, request->ticket, &reply.result)) {
		wifi->operation = request->ticket;
		wifi->connecting =
			action.action == FPLINUX_UI_WIFI_CONNECT ||
			action.action == FPLINUX_UI_WIFI_CONNECT_SAVED ||
			action.action == FPLINUX_UI_WIFI_CONNECT_HIDDEN;
		reply.ticket = 0;
	} else if (reply.result == FPLINUX_UI_WIFI_APPLIED &&
		   (request->action.action == FPLINUX_UI_WIFI_CANCEL ||
		    action.action == FPLINUX_UI_WIFI_DISCONNECT)) {
		stop_address(wifi);
		complete_wifi(wifi, FPLINUX_UI_WIFI_APPLIED);
	}
clear:
	explicit_bzero(action.key, sizeof(action.key));
	return reply;
}

static void wifi_address(struct wifi_worker *wifi, const char *interface)
{
	if (!wifi->association) {
		wifi->association = true;
		wifi->dhcp = fplinux_session_wifi_dhcp_start(interface);
		wifi->dhcp_state = wifi->dhcp ?
					   FPLINUX_SESSION_WIFI_DHCP_WAITING :
					   FPLINUX_SESSION_WIFI_DHCP_FAILED;
	}
	if (wifi->dhcp)
		wifi->dhcp_state = fplinux_session_wifi_dhcp_read(
			wifi->dhcp, wifi->address);
}

static struct wifi_result
update_wifi(struct wifi_worker *wifi,
	    const struct wireless_interface *interface,
	    struct fplinux_ui_wifi_state *state)
{
	struct wifi_result reply = { 0 };
	const struct fplinux_ui_wifi_state *station_state;
	uint32_t ticket;

	if (!wifi->station)
		return reply;
	fplinux_session_wifi_station_update(wifi->station);
	station_state = fplinux_session_wifi_station_state(wifi->station);
	*state = *station_state;
	state->power = FPLINUX_UI_WIFI_ON;
	if (fplinux_session_wifi_station_take_result(wifi->station, &ticket,
						     &reply.result) &&
	    wifi->operation && ticket == wifi->operation) {
		if (wifi->connecting &&
		    state->link == FPLINUX_UI_WIFI_ADDRESSING &&
		    reply.result == FPLINUX_UI_WIFI_APPLIED) {
			wifi->waiting_for_address = true;
		} else {
			reply = complete_wifi(wifi, reply.result);
		}
	}
	if (state->link != FPLINUX_UI_WIFI_ADDRESSING) {
		stop_address(wifi);
		if (wifi->waiting_for_address)
			return complete_wifi(
				wifi, FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE);
		return reply;
	}
	wifi_address(wifi, interface->name);
	if (wifi->waiting_for_address &&
	    wifi->dhcp_state != FPLINUX_SESSION_WIFI_DHCP_WAITING) {
		enum fplinux_ui_wifi_result result =
			fplinux_session_wifi_station_commit(wifi->station);

		*state = *station_state;
		state->power = FPLINUX_UI_WIFI_ON;
		if (result == FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE) {
			stop_address(wifi);
			state->link = FPLINUX_UI_WIFI_DISCONNECTED;
			state->current = (struct fplinux_ui_wifi_network){ 0 };
			return complete_wifi(wifi, result);
		}
		if (result == FPLINUX_UI_WIFI_APPLIED &&
		    wifi->dhcp_state == FPLINUX_SESSION_WIFI_DHCP_FAILED)
			result = FPLINUX_UI_WIFI_ADDRESS_FAILED;
		reply = complete_wifi(wifi, result);
	}
	if (wifi->dhcp_state == FPLINUX_SESSION_WIFI_DHCP_BOUND) {
		state->link = FPLINUX_UI_WIFI_CONNECTED;
		memcpy(state->address, wifi->address, sizeof(state->address));
	} else if (wifi->dhcp_state == FPLINUX_SESSION_WIFI_DHCP_FAILED) {
		state->link = FPLINUX_UI_WIFI_NO_ADDRESS;
	}
	return reply;
}

static void *worker(void *context)
{
	struct fplinux_session_wireless *wireless = context;
	struct fplinux_session_wireless_state state = { 0 };
	struct fplinux_ui_wifi_state wifi_state = { 0 };
	struct wireless_interface interface = { 0 };
	struct wifi_worker wifi = { 0 };
	bool has_supplicant = access("/sbin/wpa_supplicant", X_OK) == 0;
	struct pollfd fds[2] = { { .fd = wireless->command[0],
				   .events = POLLIN },
				 { .events = POLLIN } };
	int listener_error = 0;

	fds[1].fd = open_listener();
	if (fds[1].fd < 0) {
		listener_error = errno;
		state.error = listener_error;
	} else {
		snapshot(&state, &interface);
	}
	refresh_wifi(&wifi, &interface, &state, has_supplicant);
	wifi_state.power = has_supplicant && state.known ?
				   (state.enabled ? FPLINUX_UI_WIFI_ON :
						    FPLINUX_UI_WIFI_OFF) :
				   FPLINUX_UI_WIFI_UNAVAILABLE;
	if (wifi.station)
		update_wifi(&wifi, &interface, &wifi_state);
	publish(wireless, &state, &wifi_state, (struct wifi_result){ 0 }, false,
		0, true);
	for (;;) {
		struct wifi_request request;
		struct wifi_result reply = { 0 };
		struct wifi_result updated;
		bool stop, pending, requested;
		int ready, error = 0;
		int timeout = wifi.station ?
				      (wifi.operation ? WIFI_ACTIVE_POLL_MS :
							WIFI_IDLE_POLL_MS) :
				      -1;

		do {
			ready = poll(fds, 2, timeout);
		} while (ready < 0 && errno == EINTR);
		if (ready < 0) {
			listener_error = errno;
			state = (struct fplinux_session_wireless_state){
				.error = listener_error
			};
			pthread_mutex_lock(&wireless->mutex);
			wireless->state = state;
			wireless->wifi_state =
				(struct fplinux_ui_wifi_state){ 0 };
			wireless->worker_error = listener_error;
			if (wireless->wifi_request.ticket || wifi.operation)
				wireless->wifi_result = (struct wifi_result){
					.ticket =
						wireless->wifi_request.ticket ?
							wireless->wifi_request
								.ticket :
							wifi.operation,
					.result = FPLINUX_UI_WIFI_FAILED,
				};
			explicit_bzero(&wireless->wifi_request,
				       sizeof(wireless->wifi_request));
			if (wireless->pending) {
				wireless->pending = false;
				wireless->ready = true;
				wireless->result_error = listener_error;
			}
			pthread_mutex_unlock(&wireless->mutex);
			wake(wireless->notification[1]);
			break;
		}
		if (fds[0].revents)
			drain(fds[0].fd);
		pthread_mutex_lock(&wireless->mutex);
		stop = wireless->stop;
		pending = wireless->pending;
		requested = wireless->requested;
		request = wireless->wifi_request;
		explicit_bzero(&wireless->wifi_request,
			       sizeof(wireless->wifi_request));
		pthread_mutex_unlock(&wireless->mutex);
		if (stop) {
			if (pending && !listener_error) {
				if (!requested)
					stop_wifi(&wifi);
				operate(requested, &state, &interface);
			}
			explicit_bzero(request.action.key,
				       sizeof(request.action.key));
			break;
		}
		if (fds[1].revents) {
			listener_error = drain_listener(fds[1].fd);
			if (listener_error) {
				close(fds[1].fd);
				fds[1].fd = -1;
				state = (struct fplinux_session_wireless_state){
					.error = listener_error
				};
			} else {
				snapshot(&state, &interface);
			}
		}
		if (pending ||
		    (request.ticket &&
		     request.action.action == FPLINUX_UI_WIFI_SET_POWER)) {
			bool enable = pending ? requested :
						request.action.enabled;

			if (!pending && (!has_supplicant || !state.known)) {
				error = ENODEV;
			} else if (listener_error) {
				error = listener_error;
			} else if (!enable && wifi.station &&
				   fplinux_session_wifi_station_acquire(
					   wifi.station) ==
					   FPLINUX_UI_WIFI_BUSY) {
				error = EBUSY;
			} else {
				if (!enable)
					reply = stop_wifi(&wifi);
				error = operate(enable, &state, &interface);
			}
		}
		updated =
			refresh_wifi(&wifi, &interface, &state, has_supplicant);
		if (updated.ticket)
			reply = updated;
		if (request.ticket) {
			reply = request.action.action ==
						FPLINUX_UI_WIFI_SET_POWER ?
					(struct wifi_result){
						.ticket = request.ticket,
						.result =
							error == EBUSY ?
								FPLINUX_UI_WIFI_BUSY :
							error ? FPLINUX_UI_WIFI_FAILED :
								FPLINUX_UI_WIFI_APPLIED
					} :
					request_wifi(&wifi, &request);
			explicit_bzero(request.action.key,
				       sizeof(request.action.key));
		}
		wifi_state = (struct fplinux_ui_wifi_state){ 0 };
		wifi_state.power = has_supplicant && state.known ?
					   (state.enabled ?
						    FPLINUX_UI_WIFI_ON :
						    FPLINUX_UI_WIFI_OFF) :
					   FPLINUX_UI_WIFI_UNAVAILABLE;
		updated = update_wifi(&wifi, &interface, &wifi_state);
		if (updated.ticket)
			reply = updated;
		publish(wireless, &state, &wifi_state, reply, pending, error,
			fds[1].revents != 0);
	}
	stop_wifi(&wifi);
	if (fds[1].fd >= 0)
		close(fds[1].fd);
	return NULL;
}

struct fplinux_session_wireless *fplinux_session_wireless_create(void)
{
	struct fplinux_session_wireless *wireless =
		calloc(1, sizeof(*wireless));
	int error;

	if (!wireless)
		return NULL;
	wireless->command[0] = wireless->command[1] = -1;
	wireless->notification[0] = wireless->notification[1] = -1;
	error = pthread_mutex_init(&wireless->mutex, NULL);
	if (error)
		goto fail;
	if (pipe2(wireless->command, O_CLOEXEC | O_NONBLOCK) < 0 ||
	    pipe2(wireless->notification, O_CLOEXEC | O_NONBLOCK) < 0) {
		error = errno;
		goto fail_mutex;
	}
	error = pthread_create(&wireless->thread, NULL, worker, wireless);
	if (error)
		goto fail_mutex;
	return wireless;
fail_mutex:
	if (wireless->notification[0] >= 0) {
		close(wireless->notification[0]);
		close(wireless->notification[1]);
	}
	if (wireless->command[0] >= 0) {
		close(wireless->command[0]);
		close(wireless->command[1]);
	}
	pthread_mutex_destroy(&wireless->mutex);
fail:
	free(wireless);
	errno = error;
	return NULL;
}

void fplinux_session_wireless_destroy(struct fplinux_session_wireless *wireless)
{
	if (!wireless)
		return;
	pthread_mutex_lock(&wireless->mutex);
	wireless->stop = true;
	pthread_mutex_unlock(&wireless->mutex);
	wake(wireless->command[1]);
	pthread_join(wireless->thread, NULL);
	close(wireless->notification[0]);
	close(wireless->notification[1]);
	close(wireless->command[0]);
	close(wireless->command[1]);
	pthread_mutex_destroy(&wireless->mutex);
	free(wireless);
}

int fplinux_session_wireless_fd(const struct fplinux_session_wireless *wireless)
{
	return wireless ? wireless->notification[0] : -1;
}

void fplinux_session_wireless_dispatch(struct fplinux_session_wireless *wireless)
{
	if (wireless)
		drain(wireless->notification[0]);
}

struct fplinux_session_wireless_state
fplinux_session_wireless_state(struct fplinux_session_wireless *wireless)
{
	struct fplinux_session_wireless_state state = { .error = EINVAL };

	if (!wireless)
		return state;
	pthread_mutex_lock(&wireless->mutex);
	state = wireless->state;
	pthread_mutex_unlock(&wireless->mutex);
	return state;
}

int fplinux_session_wireless_request(struct fplinux_session_wireless *wireless,
				     bool enabled)
{
	int error;

	if (!wireless)
		return EINVAL;
	pthread_mutex_lock(&wireless->mutex);
	error = wireless->worker_error;
	if (error) {
		pthread_mutex_unlock(&wireless->mutex);
		return error;
	}
	if (wireless->pending || wireless->ready ||
	    wireless->wifi_request.ticket) {
		pthread_mutex_unlock(&wireless->mutex);
		return EBUSY;
	}
	wireless->pending = true;
	wireless->requested = enabled;
	pthread_mutex_unlock(&wireless->mutex);
	wake(wireless->command[1]);
	return 0;
}

bool fplinux_session_wireless_take_result(
	struct fplinux_session_wireless *wireless, int *error)
{
	bool ready;

	if (!wireless || !error)
		return false;
	pthread_mutex_lock(&wireless->mutex);
	ready = wireless->ready;
	if (ready) {
		*error = wireless->result_error;
		wireless->ready = false;
	}
	pthread_mutex_unlock(&wireless->mutex);
	return ready;
}

void fplinux_session_wireless_wifi_state(
	struct fplinux_session_wireless *wireless,
	struct fplinux_ui_wifi_state *state)
{
	if (!wireless) {
		*state = (struct fplinux_ui_wifi_state){ 0 };
		return;
	}
	pthread_mutex_lock(&wireless->mutex);
	*state = wireless->wifi_state;
	pthread_mutex_unlock(&wireless->mutex);
}

int fplinux_session_wireless_wifi_request(
	struct fplinux_session_wireless *wireless,
	const struct fplinux_ui_wifi_request *request, uint32_t ticket)
{
	bool cancel_queued;
	int error;

	if (!wireless || !request || !ticket)
		return EINVAL;
	pthread_mutex_lock(&wireless->mutex);
	cancel_queued =
		request->action == FPLINUX_UI_WIFI_CANCEL && request->target &&
		request->target == wireless->wifi_request.ticket &&
		wireless->wifi_request.action.action != FPLINUX_UI_WIFI_CANCEL;
	error = wireless->worker_error;
	if (!error && (wireless->pending || wireless->ready ||
		       (wireless->wifi_request.ticket && !cancel_queued) ||
		       wireless->wifi_result.ticket))
		error = EBUSY;
	if (!error) {
		if (cancel_queued) {
			/* The worker has no state to undo before dequeueing. */
			explicit_bzero(&wireless->wifi_request,
				       sizeof(wireless->wifi_request));
			wireless->wifi_result = (struct wifi_result){
				.ticket = ticket,
				.result = FPLINUX_UI_WIFI_APPLIED,
			};
		} else {
			wireless->wifi_request = (struct wifi_request){
				.ticket = ticket,
				.action = *request,
			};
		}
	}
	pthread_mutex_unlock(&wireless->mutex);
	if (!error)
		wake(cancel_queued ? wireless->notification[1] :
				     wireless->command[1]);
	return error;
}

bool fplinux_session_wireless_take_wifi_result(
	struct fplinux_session_wireless *wireless, uint32_t *ticket,
	enum fplinux_ui_wifi_result *result)
{
	struct wifi_result reply;

	if (!wireless)
		return false;
	pthread_mutex_lock(&wireless->mutex);
	reply = wireless->wifi_result;
	wireless->wifi_result.ticket = 0;
	pthread_mutex_unlock(&wireless->mutex);
	if (!reply.ticket)
		return false;
	*ticket = reply.ticket;
	*result = reply.result;
	return true;
}
