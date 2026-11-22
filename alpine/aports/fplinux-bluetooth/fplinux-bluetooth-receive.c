/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-bluetooth-receive.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "fplinux-bluetooth-common.h"
#include "fplinux-bluetooth-opp.h"
#include "fplinux-bluez-adapter.h"

#define LEASE_GRANTED 'G'
#define LEASE_BUSY 'B'
#define LEASE_TIMEOUT_MS 3000
#define PROGRESS_INTERVAL_MS 250

struct worker {
	struct fplinux_bluez_adapter *adapter;
	const volatile sig_atomic_t *interrupted;
	int lease;
	bool lease_granted;
	bool registered;
	bool stopped;
	bool offer_invalidated;
	bool status_known;
	enum fplinux_session_bluetooth_state status;
	enum fplinux_bluetooth_opp_consent consent;
	uint32_t next_id;
	int64_t last_progress_ms;
	char files_directory[PATH_MAX];
	char owner[256];
	char local[18];
	char remote[18];
	struct fplinux_session_bluetooth_event offer;
};

int fplinux_bluetooth_receive_claim(int *lease,
				    const volatile sig_atomic_t *interrupted)
{
	const struct sockaddr_un address = {
		.sun_family = AF_UNIX,
		.sun_path = FPLINUX_BLUETOOTH_RECEIVE_SOCKET,
	};
	*lease = -1;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
			0);
	if (fd < 0)
		return errno;
	int error = 0;
	if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) <
	    0) {
		error = errno;
		if (error == ENOENT || error == ECONNREFUSED)
			error = 0;
		else if (error == EAGAIN)
			error = EBUSY;
		goto close_fd;
	}
	int64_t deadline = fplinux_bluetooth_now_ms() + LEASE_TIMEOUT_MS;
	while (*interrupted == 0) {
		int64_t remaining = deadline - fplinux_bluetooth_now_ms();
		if (remaining <= 0) {
			error = ETIMEDOUT;
			goto close_fd;
		}
		struct pollfd poll_fd = { .fd = fd, .events = POLLIN };
		int ready =
			poll(&poll_fd, 1, remaining > 50 ? 50 : (int)remaining);
		if (ready < 0 && errno != EINTR) {
			error = errno;
			goto close_fd;
		}
		if (ready > 0) {
			char reply;
			ssize_t bytes =
				recv(fd, &reply, sizeof(reply), MSG_TRUNC);
			if (bytes == sizeof(reply) && reply == LEASE_GRANTED) {
				*lease = fd;
				return 0;
			}
			error = bytes == sizeof(reply) && reply == LEASE_BUSY ?
					EBUSY :
					EIO;
			goto close_fd;
		}
	}
	error = ECANCELED;
close_fd:
	close(fd);
	return error;
}

static void send_event(struct worker *worker,
		       const struct fplinux_session_bluetooth_event *event)
{
	if (worker->stopped)
		return;
	ssize_t bytes;
	do {
		bytes = send(FPLINUX_BLUETOOTH_RECEIVE_CONTROL_FD, event,
			     sizeof(*event), MSG_NOSIGNAL);
	} while (bytes < 0 && errno == EINTR);
	if (bytes != sizeof(*event))
		worker->stopped = true;
}

static bool cancelled(void *data)
{
	struct worker *worker = data;
	if (*worker->interrupted != 0 || worker->stopped)
		return true;
	struct fplinux_bluetooth_receive_decision decision;
	ssize_t bytes;
	while ((bytes = recv(FPLINUX_BLUETOOTH_RECEIVE_CONTROL_FD, &decision,
			     sizeof(decision), MSG_TRUNC)) ==
	       sizeof(decision)) {
		if (worker->offer.id != 0 && worker->offer.id == decision.id &&
		    worker->consent == FPLINUX_BLUETOOTH_OPP_PENDING)
			worker->consent = decision.accept ?
						  FPLINUX_BLUETOOTH_OPP_ACCEPT :
						  FPLINUX_BLUETOOTH_OPP_REJECT;
	}
	if (bytes >= 0 ||
	    (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
		worker->stopped = true;
	return worker->stopped;
}

static void update_status(struct worker *worker)
{
	enum fplinux_session_bluetooth_state status;
	if (!fplinux_bluez_adapter_present(worker->adapter))
		status = FPLINUX_SESSION_BLUETOOTH_UNAVAILABLE;
	else if (!fplinux_bluez_adapter_powered(worker->adapter))
		status = FPLINUX_SESSION_BLUETOOTH_OFF;
	else if (worker->lease >= 0)
		status = FPLINUX_SESSION_BLUETOOTH_BUSY;
	else if (worker->registered)
		status = FPLINUX_SESSION_BLUETOOTH_READY;
	else
		status = FPLINUX_SESSION_BLUETOOTH_UNAVAILABLE;
	if (worker->status_known && status == worker->status)
		return;
	worker->status_known = true;
	worker->status = status;
	struct fplinux_session_bluetooth_event event = { .state = status };
	send_event(worker, &event);
}

static const struct fplinux_bluez_device *
find_peer(struct worker *worker, const char *local, const char *remote)
{
	if (!fplinux_bluez_adapter_present(worker->adapter) ||
	    !fplinux_bluez_adapter_powered(worker->adapter) ||
	    local[0] == '\0' || remote[0] == '\0' ||
	    strcasecmp(local, fplinux_bluez_adapter_address(worker->adapter)) !=
		    0)
		return NULL;
	size_t count = fplinux_bluez_adapter_device_count(worker->adapter);
	for (size_t index = 0; index < count; index++) {
		const struct fplinux_bluez_device *device =
			fplinux_bluez_adapter_device_at(worker->adapter, index);
		if (device->paired && strcasecmp(device->address, remote) == 0)
			return device;
	}
	return NULL;
}

static void adapter_changed(void *data)
{
	struct worker *worker = data;
	if (worker->remote[0] &&
	    (strcmp(worker->owner,
		    fplinux_bluez_adapter_owner(worker->adapter)) != 0 ||
	     find_peer(worker, worker->local, worker->remote) == NULL))
		worker->offer_invalidated = true;
	update_status(worker);
}

static bool peer_allowed(const char *local, const char *remote, char *name,
			 size_t name_size, void *data)
{
	struct worker *worker = data;
	fplinux_bluez_adapter_dispatch(worker->adapter);
	const struct fplinux_bluez_device *device =
		find_peer(worker, local, remote);
	if (device == NULL)
		return false;
	const char *owner = fplinux_bluez_adapter_owner(worker->adapter);
	if (name == NULL)
		return !worker->offer_invalidated &&
		       strcmp(worker->owner, owner) == 0;
	if (owner[0] == '\0')
		return false;
	snprintf(worker->owner, sizeof(worker->owner), "%s", owner);
	snprintf(worker->local, sizeof(worker->local), "%s", local);
	snprintf(worker->remote, sizeof(worker->remote), "%s", remote);
	worker->offer_invalidated = false;
	const char *label = device->alias[0] ? device->alias : device->name;
	if (!label[0] || !dbus_validate_utf8(label, NULL))
		label = remote;
	snprintf(name, name_size, "%s", label);
	return true;
}

static enum fplinux_bluetooth_opp_consent
authorize(const struct fplinux_bluetooth_opp_offer *offer, void *data)
{
	struct worker *worker = data;
	if (worker->offer.id != 0) {
		if (worker->consent == FPLINUX_BLUETOOTH_OPP_ACCEPT)
			mkdir(worker->files_directory, 0700);
		return worker->consent;
	}
	if (++worker->next_id == 0)
		++worker->next_id;
	worker->offer = (struct fplinux_session_bluetooth_event){
		.state = FPLINUX_SESSION_BLUETOOTH_OFFER,
		.id = worker->next_id,
		.total_bytes = offer->total_bytes,
		.total_known = offer->total_known,
	};
	snprintf(worker->offer.peer, sizeof(worker->offer.peer), "%s",
		 offer->peer);
	snprintf(worker->offer.filename, sizeof(worker->offer.filename), "%s",
		 offer->filename);
	worker->consent = FPLINUX_BLUETOOTH_OPP_PENDING;
	send_event(worker, &worker->offer);
	return worker->consent;
}

static void progress(const struct fplinux_bluetooth_opp_progress *progress,
		     void *data)
{
	struct worker *worker = data;
	if (worker->offer.id == 0)
		return;
	if (progress->state == FPLINUX_BLUETOOTH_OPP_COMPLETE) {
		worker->offer.transferred_bytes = progress->transferred_bytes;
		worker->offer.total_bytes = progress->total_bytes;
		worker->offer.total_known = progress->total_known;
		return;
	}
	if (progress->state != FPLINUX_BLUETOOTH_OPP_TRANSFERRING)
		return;
	int64_t now = fplinux_bluetooth_now_ms();
	if (worker->offer.state == FPLINUX_SESSION_BLUETOOTH_TRANSFERRING &&
	    now - worker->last_progress_ms < PROGRESS_INTERVAL_MS)
		return;
	worker->last_progress_ms = now;
	worker->offer.state = FPLINUX_SESSION_BLUETOOTH_TRANSFERRING;
	worker->offer.transferred_bytes = progress->transferred_bytes;
	worker->offer.total_bytes = progress->total_bytes;
	worker->offer.total_known = progress->total_known;
	send_event(worker, &worker->offer);
}

static void finished(bool accepted,
		     const struct fplinux_bluetooth_opp_result *result,
		     void *data)
{
	struct worker *worker = data;
	worker->remote[0] = '\0';
	if (worker->offer.id == 0)
		return;
	if (!accepted) {
		worker->offer.state = FPLINUX_SESSION_BLUETOOTH_CLEARED;
	} else if (result->outcome == FPLINUX_BLUETOOTH_OPP_SUCCEEDED) {
		worker->offer.state = FPLINUX_SESSION_BLUETOOTH_COMPLETE;
		snprintf(worker->offer.path, sizeof(worker->offer.path), "%s",
			 result->path);
		const char *name = strrchr(result->path, '/');
		if (name != NULL)
			snprintf(worker->offer.filename,
				 sizeof(worker->offer.filename), "%s",
				 name + 1);
	} else {
		worker->offer.state = FPLINUX_SESSION_BLUETOOTH_FAILED;
	}
	if (result->message[0] != '\0' && accepted)
		fplinux_bluetooth_fail("%s", result->message);
	send_event(worker, &worker->offer);
	worker->offer.id = 0;
}

static void close_lease(struct worker *worker)
{
	close(worker->lease);
	worker->lease = -1;
	worker->lease_granted = false;
}

static bool suspended(bool busy, void *data)
{
	struct worker *worker = data;
	fplinux_bluez_adapter_connect(worker->adapter);
	fplinux_bluez_adapter_dispatch(worker->adapter);
	if (worker->lease >= 0) {
		char input;
		if (recv(worker->lease, &input, sizeof(input), MSG_PEEK) >= 0 ||
		    (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			close_lease(worker);
	}
	int candidate = accept4(FPLINUX_BLUETOOTH_RECEIVE_LISTENER_FD, NULL,
				NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (candidate >= 0) {
		if (busy || worker->lease >= 0) {
			send(candidate, (const char[]){ LEASE_BUSY }, 1,
			     MSG_NOSIGNAL);
			close(candidate);
		} else {
			worker->lease = candidate;
		}
	}
	update_status(worker);
	return worker->lease >= 0;
}

static void registered(bool ready, void *data)
{
	struct worker *worker = data;
	worker->registered = ready;
	if (!ready && worker->lease >= 0 && !worker->lease_granted) {
		if (send(worker->lease, (const char[]){ LEASE_GRANTED }, 1,
			 MSG_NOSIGNAL) == 1)
			worker->lease_granted = true;
		else
			shutdown(worker->lease, SHUT_RDWR);
	}
	update_status(worker);
}

static void wait_for_event(int bus_fd, int timeout_ms, void *data)
{
	struct worker *worker = data;
	struct pollfd descriptors[] = {
		{ .fd = FPLINUX_BLUETOOTH_RECEIVE_CONTROL_FD,
		  .events = POLLIN },
		{ .fd = FPLINUX_BLUETOOTH_RECEIVE_LISTENER_FD,
		  .events = POLLIN },
		{ .fd = fplinux_bluez_adapter_fd(worker->adapter),
		  .events = POLLIN },
		{ .fd = worker->lease, .events = POLLIN },
		{ .fd = bus_fd, .events = POLLIN },
	};
	poll(descriptors, sizeof(descriptors) / sizeof(descriptors[0]),
	     timeout_ms);
}

int fplinux_bluetooth_receive_worker(const char *home,
				     const volatile sig_atomic_t *interrupted)
{
	struct worker worker = { .interrupted = interrupted, .lease = -1 };
	char directory[PATH_MAX];
	if (home[0] != '/' ||
	    snprintf(worker.files_directory, sizeof(worker.files_directory),
		     "%s/Files", home) >= (int)sizeof(worker.files_directory) ||
	    snprintf(directory, sizeof(directory), "%s/Bluetooth",
		     worker.files_directory) >= (int)sizeof(directory))
		return fplinux_bluetooth_fail("invalid Bluetooth receive home");
	if (fcntl(FPLINUX_BLUETOOTH_RECEIVE_CONTROL_FD, F_GETFD) < 0 ||
	    fcntl(FPLINUX_BLUETOOTH_RECEIVE_LISTENER_FD, F_GETFD) < 0)
		return fplinux_bluetooth_fail(
			"Bluetooth receive requires a graphical session");
	const struct fplinux_bluez_adapter_listener adapter_listener = {
		.changed = adapter_changed,
		.context = &worker,
	};
	worker.adapter = fplinux_bluez_adapter_open(&adapter_listener);
	if (worker.adapter == NULL)
		return fplinux_bluetooth_fail(
			"cannot initialize Bluetooth adapter monitor: %s",
			strerror(errno));
	struct fplinux_bluetooth_opp_callbacks callbacks = {
		.cancelled = cancelled,
		.progress = progress,
		.authorize = authorize,
		.data = &worker,
	};
	const struct fplinux_bluetooth_opp_listener listener = {
		.suspended = suspended,
		.peer_allowed = peer_allowed,
		.registered = registered,
		.finished = finished,
		.wait = wait_for_event,
	};
	fplinux_bluetooth_opp_listen(directory, &callbacks, &listener);
	if (worker.lease >= 0)
		close_lease(&worker);
	fplinux_bluez_adapter_close(worker.adapter);
	return 0;
}
