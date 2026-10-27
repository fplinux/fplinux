/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * Linux notification adapter for the upstream PCSL BSD sockets. Only the
 * VM thread owns this list and MIDP reentry data. A resolver child writes a
 * fixed result to a pipe; it never touches the VM or the GUI connection.
 */
#define _GNU_SOURCE
#include "fplinux-pm-network.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <midp_thread.h>
#include <midpServices.h>
#include <midp_net_events.h>
#include <pcsl_network.h>
#include <pcsl_network_na.h>
#include <sni.h>

extern int lastError;

struct resolver_result {
	int error;
	unsigned char address[4];
};

struct network_handle {
	struct network_handle *next;
	int id;
	int fd;
	pid_t resolver;
	bool is_lookup;
	bool canceled;
};

static struct network_handle *handles;
static bool network_canceled;
/* MIDP stores handles in jint. Never reuse an id for a later socket/wait. */
static int next_id = 1;

static struct network_handle *find_handle(void *handle)
{
	for (struct network_handle *item = handles; item; item = item->next) {
		if ((uintptr_t)handle == (uintptr_t)item->id)
			return item;
	}
	return NULL;
}

static void remove_handle(struct network_handle *item)
{
	struct network_handle **link = &handles;

	while (*link != item)
		link = &(*link)->next;
	*link = item->next;
	free(item);
}

static void reap_resolvers(void)
{
	struct network_handle *item = handles;

	while (item) {
		struct network_handle *next = item->next;

		if (item->resolver > 0) {
			pid_t result = waitpid(item->resolver, NULL, WNOHANG);

			if (result == item->resolver ||
			    (result < 0 && errno == ECHILD))
				item->resolver = 0;
		}
		if (item->canceled && item->resolver == 0)
			remove_handle(item);
		item = next;
	}
}

static void stop_resolver(struct network_handle *item)
{
	pid_t result;

	if (item->resolver <= 0)
		return;
	/* SIGKILL ends a lookup immediately, rather than waiting for DNS. */
	kill(item->resolver, SIGKILL);
	do {
		result = waitpid(item->resolver, NULL, 0);
	} while (result < 0 && errno == EINTR);
	item->resolver = 0;
}

static struct network_handle *create_handle(int fd, bool is_lookup)
{
	struct network_handle *item;

	reap_resolvers();
	if (network_canceled || next_id == INT_MAX) {
		lastError = network_canceled ? ECANCELED : EMFILE;
		return NULL;
	}
	item = calloc(1, sizeof(*item));
	if (!item) {
		lastError = ENOMEM;
		return NULL;
	}
	item->id = next_id++;
	item->fd = fd;
	item->is_lookup = is_lookup;
	item->next = handles;
	handles = item;
	return item;
}

void *na_create(int fd)
{
	struct network_handle *item = create_handle(fd, false);

	return item ? (void *)(uintptr_t)item->id : NULL;
}

int na_get_fd(void *handle)
{
	struct network_handle *item = find_handle(handle);

	return item && !item->canceled ? item->fd : -1;
}

int na_get_status(void *handle)
{
	struct network_handle *item = find_handle(handle);

	return item && !item->canceled ? PCSL_NET_SUCCESS :
					 PCSL_NET_INTERRUPTED;
}

void na_destroy(void *handle)
{
	struct network_handle *item = find_handle(handle);

	/* PCSL closes the fd before destroying its notification adapter. */
	if (item && !item->is_lookup)
		remove_handle(item);
}

/* The current MIDP blocked-thread snapshot is the readiness registration. */
void na_register_for_read(void *handle)
{
	(void)handle;
}

void na_register_for_write(void *handle)
{
	(void)handle;
}

void na_unregister_for_read(void *handle)
{
	(void)handle;
}

void na_unregister_for_write(void *handle)
{
	(void)handle;
}

static void child_close_descriptors(int result_fd, rlim_t descriptor_limit)
{
	DIR *directory = opendir("/proc/self/fd");
	struct dirent *entry;

	if (!directory) {
		for (rlim_t fd = 0; fd < descriptor_limit; ++fd) {
			if (fd != (rlim_t)result_fd)
				close((int)fd);
		}
		return;
	}
	while ((entry = readdir(directory))) {
		char *end;
		long fd = strtol(entry->d_name, &end, 10);

		if (*end == '\0' && fd >= 0 && fd <= INT_MAX &&
		    fd != result_fd && fd != dirfd(directory))
			close((int)fd);
	}
	closedir(directory);
}

static void resolve_in_child(const char *hostname, int result_fd,
			     rlim_t descriptor_limit)
{
	struct addrinfo hints = { .ai_family = AF_INET,
				  .ai_socktype = SOCK_STREAM };
	struct addrinfo *addresses = NULL;
	struct resolver_result result = { .error = EHOSTUNREACH };
	ssize_t written;

	child_close_descriptors(result_fd, descriptor_limit);
	if (getaddrinfo(hostname, NULL, &hints, &addresses) == 0 && addresses) {
		struct sockaddr_in *address =
			(struct sockaddr_in *)addresses->ai_addr;

		memcpy(result.address, &address->sin_addr,
		       sizeof(result.address));
		result.error = 0;
	}
	if (addresses)
		freeaddrinfo(addresses);
	do {
		written = write(result_fd, &result, sizeof(result));
	} while (written < 0 && errno == EINTR);
	close(result_fd);
	_exit(0);
}

int fplinux_pm_network_lookup_start(char *hostname, unsigned char *address,
				    int capacity, int *length, void **handle,
				    void **context)
{
	struct network_handle *item;
	struct rlimit limit;
	int pipe_fds[2];
	struct in_addr numeric;

	*handle = NULL;
	*context = NULL;
	if (network_canceled) {
		lastError = ECANCELED;
		return PCSL_NET_IOERROR;
	}
	if (capacity < 4)
		return PCSL_NET_INVALID;
	if (inet_pton(AF_INET, hostname, &numeric) == 1) {
		memcpy(address, &numeric, 4);
		*length = 4;
		return PCSL_NET_SUCCESS;
	}
	if (getrlimit(RLIMIT_NOFILE, &limit) < 0 ||
	    pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0) {
		lastError = errno;
		return PCSL_NET_IOERROR;
	}
	item = create_handle(pipe_fds[0], true);
	if (!item) {
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		return PCSL_NET_IOERROR;
	}
	item->resolver = fork();
	if (item->resolver == 0)
		resolve_in_child(hostname, pipe_fds[1],
				 limit.rlim_cur < INT_MAX ? limit.rlim_cur :
							    INT_MAX);
	if (item->resolver < 0) {
		lastError = errno;
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		remove_handle(item);
		return PCSL_NET_IOERROR;
	}
	close(pipe_fds[1]);
	*handle = (void *)(uintptr_t)item->id;
	return PCSL_NET_WOULDBLOCK;
}

int fplinux_pm_network_lookup_finish(unsigned char *address, int capacity,
				     int *length, void *handle, void *context)
{
	struct network_handle *item = find_handle(handle);
	struct resolver_result result;
	ssize_t count;
	int status;

	(void)context;
	if (!item || item->canceled || !item->is_lookup)
		return PCSL_NET_IOERROR;
	do {
		count = read(item->fd, &result, sizeof(result));
	} while (count < 0 && errno == EINTR);
	if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		return PCSL_NET_WOULDBLOCK;
	}
	status = PCSL_NET_IOERROR;
	lastError = count == (ssize_t)sizeof(result) ? result.error :
						       EHOSTUNREACH;
	if (count == (ssize_t)sizeof(result) && result.error == 0) {
		if (capacity >= 4) {
			memcpy(address, result.address, 4);
			*length = 4;
			status = PCSL_NET_SUCCESS;
		} else {
			status = PCSL_NET_INVALID;
		}
	}
	close(item->fd);
	stop_resolver(item);
	remove_handle(item);
	return status;
}

static MidpReentryData *network_wait(JVMSPI_BlockedThreadInfo *thread)
{
	MidpReentryData *info = thread->reentry_data;

	if (!info || thread->reentry_data_size != sizeof(*info))
		return NULL;
	if (info->waitingFor == NETWORK_READ_SIGNAL ||
	    info->waitingFor == NETWORK_WRITE_SIGNAL ||
	    info->waitingFor == HOST_NAME_LOOKUP_SIGNAL)
		return info;
	return NULL;
}

bool fplinux_pm_network_has_waiters(JVMSPI_BlockedThreadInfo *threads,
				    int count)
{
	for (int i = 0; i < count; ++i) {
		if (network_wait(&threads[i]))
			return true;
	}
	return false;
}

bool fplinux_pm_network_check_events(JVMSPI_BlockedThreadInfo *threads,
				     int count)
{
	bool ready = false;

	reap_resolvers();
	for (int i = 0; i < count; ++i) {
		MidpReentryData *info = network_wait(&threads[i]);
		struct network_handle *item;
		struct pollfd descriptor;
		bool is_lookup;

		if (!info)
			continue;
		is_lookup = info->waitingFor == HOST_NAME_LOOKUP_SIGNAL;
		item = find_handle((void *)(uintptr_t)info->descriptor);
		if (!item || item->canceled || item->is_lookup != is_lookup) {
			info->status = PCSL_NET_INTERRUPTED;
			midp_thread_unblock(threads[i].thread_id);
			ready = true;
			continue;
		}
		descriptor = (struct pollfd){
			.fd = item->fd,
			.events = info->waitingFor == NETWORK_WRITE_SIGNAL ?
					  POLLOUT :
					  POLLIN,
		};
		if (poll(&descriptor, 1, 0) <= 0 || descriptor.revents == 0)
			continue;
		info->status = 0;
		midp_thread_unblock(threads[i].thread_id);
		ready = true;
	}
	return ready;
}

void fplinux_pm_network_cleanup(void)
{
	struct network_handle *item = handles;

	while (item) {
		struct network_handle *next = item->next;

		if (!item->is_lookup && item->fd >= 0) {
			struct linger immediate = { 0 };

			/* Cancellation must not wait for a MIDlet's linger timeout. */
			setsockopt(item->fd, SOL_SOCKET, SO_LINGER, &immediate,
				   sizeof(immediate));
		}
		if (item->fd >= 0)
			close(item->fd);
		item->fd = -1;
		item->canceled = true;
		stop_resolver(item);
		remove_handle(item);
		item = next;
	}
}

void fplinux_pm_network_cancel(void)
{
	JVMSPI_BlockedThreadInfo *threads;
	int count;

	network_canceled = true;
	fplinux_pm_network_cleanup();
	threads = SNI_GetBlockedThreads(&count);
	for (int i = 0; i < count; ++i) {
		MidpReentryData *info = network_wait(&threads[i]);

		if (info) {
			info->status = PCSL_NET_INTERRUPTED;
			midp_thread_unblock(threads[i].thread_id);
		}
	}
}

void midp_network_status_event(int is_init, int status)
{
	(void)is_init;
	midp_thread_signal(NETWORK_STATUS_SIGNAL, 0, status);
}
