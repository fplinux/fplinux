/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-wireless.h"

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

struct wireless_interface {
	unsigned int index;
	char name[IFNAMSIZ];
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

static unsigned int snapshot(struct fplinux_session_wireless_state *state)
{
	struct wireless_interface interface;

	memset(state, 0, sizeof(*state));
	state->error = find_interface(&interface);
	if (!state->error && interface.index) {
		state->supported = true;
		state->error = interface_flags(&interface, false, false,
					       &state->enabled);
		state->known = !state->error;
	}
	return state->error ? 0 : interface.index;
}

static int operate(bool enabled, struct fplinux_session_wireless_state *state)
{
	struct wireless_interface interface;
	unsigned int index;
	int error = find_interface(&interface);

	if (!error && !interface.index)
		error = ENODEV;
	if (!error)
		error = interface_flags(&interface, true, enabled, NULL);
	index = snapshot(state);
	if (error)
		return error;
	if (state->error)
		return state->error;
	if (!index || index != interface.index)
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
		    bool result_ready, int error)
{
	pthread_mutex_lock(&wireless->mutex);
	wireless->state = *state;
	if (result_ready) {
		wireless->pending = false;
		wireless->ready = true;
		wireless->result_error = error;
	}
	pthread_mutex_unlock(&wireless->mutex);
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

static void *worker(void *context)
{
	struct fplinux_session_wireless *wireless = context;
	struct fplinux_session_wireless_state state = { 0 };
	struct pollfd fds[2] = { { .fd = wireless->command[0],
				   .events = POLLIN },
				 { .events = POLLIN } };
	int listener_error = 0;

	fds[1].fd = open_listener();
	if (fds[1].fd < 0) {
		listener_error = errno;
		state.error = listener_error;
	} else {
		snapshot(&state);
	}
	publish(wireless, &state, false, 0);
	for (;;) {
		bool stop, pending, requested;
		int ready, error;

		do {
			ready = poll(fds, 2, -1);
		} while (ready < 0 && errno == EINTR);
		if (ready < 0) {
			listener_error = errno;
			state = (struct fplinux_session_wireless_state){
				.error = listener_error
			};
			pthread_mutex_lock(&wireless->mutex);
			wireless->state = state;
			wireless->worker_error = listener_error;
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
		pthread_mutex_unlock(&wireless->mutex);
		if (!stop && fds[1].revents) {
			listener_error = drain_listener(fds[1].fd);
			if (listener_error) {
				close(fds[1].fd);
				fds[1].fd = -1;
				state = (struct fplinux_session_wireless_state){
					.error = listener_error
				};
			} else {
				snapshot(&state);
			}
			publish(wireless, &state, false, 0);
		}
		if (pending) {
			if (listener_error) {
				error = listener_error;
			} else {
				error = operate(requested, &state);
			}
			publish(wireless, &state, true, error);
		}
		if (stop)
			break;
	}
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
	if (wireless->pending || wireless->ready) {
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
