/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-wifi-dhcp.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DHCP_HOOK "/usr/libexec/fplinux/wifi-udhcpc-hook"
#define DHCP_STATE_DIR "/run/fplinux-wifi-dhcp.XXXXXX"
#define DHCP_STATE_ENV "FPLINUX_WIFI_DHCP_STATE="
#define DHCP_STOP_GRACE_MS 1000
#define DHCP_STOP_INTERVAL_MS 20

struct fplinux_session_wifi_dhcp {
	pid_t child;
	char interface[IF_NAMESIZE];
	char directory[sizeof(DHCP_STATE_DIR)];
};

static int spawn(const struct fplinux_session_wifi_dhcp *dhcp, pid_t *child,
		 const char *path, char *const arguments[],
		 const posix_spawnattr_t *attributes)
{
	char directory[sizeof(DHCP_STATE_ENV) + sizeof(dhcp->directory)];
	char interface[sizeof("interface=") + sizeof(dhcp->interface)];
	char *environment[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", directory,
				interface, NULL };

	snprintf(directory, sizeof(directory), DHCP_STATE_ENV "%s",
		 dhcp->directory);
	snprintf(interface, sizeof(interface), "interface=%s", dhcp->interface);
	return posix_spawn(child, path, NULL, attributes, arguments,
			   environment);
}

static int start_client(struct fplinux_session_wifi_dhcp *dhcp)
{
	char *arguments[] = {
		"busybox",	"udhcpc",  "-f",     "-n", "-R",
		"-t",		"3",	   "-T",     "3",  "-O",
		"staticroutes", "-O",	   "search", "-i", dhcp->interface,
		"-s",		DHCP_HOOK, NULL,
	};
	posix_spawnattr_t attributes;
	sigset_t signals;
	int error;

	error = posix_spawnattr_init(&attributes);
	if (error)
		return error;
	sigemptyset(&signals);
	error = posix_spawnattr_setsigmask(&attributes, &signals);
	if (!error)
		error = posix_spawnattr_setpgroup(&attributes, 0);
	if (!error)
		error = posix_spawnattr_setflags(
			&attributes,
			POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
	if (!error)
		error = spawn(dhcp, &dhcp->child, "/bin/busybox", arguments,
			      &attributes);
	posix_spawnattr_destroy(&attributes);
	return error;
}

static pid_t wait_child(pid_t child, int options)
{
	pid_t result;

	do {
		result = waitpid(child, NULL, options);
	} while (result < 0 && errno == EINTR);
	return result;
}

static void stop_client(pid_t child)
{
	const struct timespec interval = {
		.tv_nsec = DHCP_STOP_INTERVAL_MS * 1000000L,
	};

	if (child <= 0)
		return;
	if (wait_child(child, WNOHANG) != 0)
		return;
	/* BusyBox -R releases the lease and runs the deconfig hook on TERM. */
	kill(child, SIGTERM);
	for (int elapsed = 0; elapsed < DHCP_STOP_GRACE_MS;
	     elapsed += DHCP_STOP_INTERVAL_MS) {
		if (wait_child(child, WNOHANG) != 0)
			return;
		nanosleep(&interval, NULL);
	}
	/* Include a still-running hook if the client could not finish. */
	kill(-child, SIGKILL);
	wait_child(child, 0);
}

struct fplinux_session_wifi_dhcp *
fplinux_session_wifi_dhcp_start(const char *interface)
{
	struct fplinux_session_wifi_dhcp *dhcp;
	int error;

	if (!interface || !*interface || strlen(interface) >= IF_NAMESIZE) {
		errno = EINVAL;
		return NULL;
	}
	dhcp = calloc(1, sizeof(*dhcp));
	if (!dhcp)
		return NULL;
	snprintf(dhcp->interface, sizeof(dhcp->interface), "%s", interface);
	memcpy(dhcp->directory, DHCP_STATE_DIR, sizeof(dhcp->directory));
	if (!mkdtemp(dhcp->directory)) {
		error = errno;
		goto fail;
	}
	error = start_client(dhcp);
	if (error) {
		rmdir(dhcp->directory);
		goto fail;
	}
	return dhcp;

fail:
	free(dhcp);
	errno = error;
	return NULL;
}

enum fplinux_session_wifi_dhcp_state
fplinux_session_wifi_dhcp_read(struct fplinux_session_wifi_dhcp *dhcp,
			       char address[INET_ADDRSTRLEN])
{
	char path[sizeof(dhcp->directory) + sizeof("/status")];
	char state[INET_ADDRSTRLEN + 1];
	struct in_addr ipv4;
	ssize_t length;
	int fd;

	address[0] = '\0';
	if (dhcp->child <= 0 || wait_child(dhcp->child, WNOHANG) != 0) {
		dhcp->child = 0;
		return FPLINUX_SESSION_WIFI_DHCP_FAILED;
	}
	snprintf(path, sizeof(path), "%s/status", dhcp->directory);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT ? FPLINUX_SESSION_WIFI_DHCP_WAITING :
					 FPLINUX_SESSION_WIFI_DHCP_FAILED;
	do {
		length = read(fd, state, sizeof(state));
	} while (length < 0 && errno == EINTR);
	close(fd);
	if (length < 2 || length >= (ssize_t)sizeof(state) ||
	    state[length - 1] != '\n')
		return FPLINUX_SESSION_WIFI_DHCP_FAILED;
	state[length - 1] = '\0';
	if (inet_pton(AF_INET, state, &ipv4) != 1)
		return FPLINUX_SESSION_WIFI_DHCP_FAILED;
	memcpy(address, state, (size_t)length);
	return FPLINUX_SESSION_WIFI_DHCP_BOUND;
}

void fplinux_session_wifi_dhcp_destroy(struct fplinux_session_wifi_dhcp *dhcp)
{
	char *arguments[] = { DHCP_HOOK, "stop", NULL };
	pid_t cleanup;

	if (!dhcp)
		return;
	stop_client(dhcp->child);
	/* Some BusyBox error paths skip the release hook. */
	if (!spawn(dhcp, &cleanup, DHCP_HOOK, arguments, NULL))
		wait_child(cleanup, 0);
	rmdir(dhcp->directory);
	free(dhcp);
}
