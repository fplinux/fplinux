/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static bool parse_number(const char *text, int *value)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno || end == text || *end || parsed < 0 || parsed > INT_MAX)
		return false;
	*value = (int)parsed;
	return true;
}

static bool restore_vt(int tty, int control, int vt, int previous_vt)
{
	struct vt_mode mode = { .mode = VT_AUTO };
	struct vt_stat state;
	bool ok = true;

	if (ioctl(tty, VT_SETMODE, &mode) < 0)
		ok = false;
	if (ioctl(tty, KDSETMODE, KD_TEXT) < 0)
		ok = false;
	if (ioctl(control, VT_GETSTATE, &state) < 0)
		return false;
	if (state.v_active == vt) {
		if (ioctl(control, VT_ACTIVATE, previous_vt) < 0 ||
		    ioctl(control, VT_WAITACTIVE, previous_vt) < 0)
			ok = false;
	}
	return ok;
}

int main(int argc, char **argv)
{
	int channel;
	int process;
	int tty;
	int control;
	int vt;
	int previous_vt;
	struct pollfd parent;
	char command;
	ssize_t count;
	int result;

	if (argc != 7 || !parse_number(argv[1], &channel) ||
	    !parse_number(argv[2], &process) || !parse_number(argv[3], &tty) ||
	    !parse_number(argv[4], &control) || !parse_number(argv[5], &vt) ||
	    !parse_number(argv[6], &previous_vt) || !vt || !previous_vt)
		return EXIT_FAILURE;
	parent.fd = process;
	parent.events = POLLIN;
	parent.revents = 0;
	if (send(channel, "R", 1, MSG_NOSIGNAL) != 1)
		return EXIT_FAILURE;
	do {
		count = read(channel, &command, 1);
	} while (count < 0 && errno == EINTR);
	if (count < 0 || (count == 1 && command != 'C'))
		return EXIT_FAILURE;
	/* EOF can precede the owner's final DRM close during exit. */
	if (count == 0) {
		do {
			result = poll(&parent, 1, -1);
		} while (result < 0 && errno == EINTR);
		if (result != 1 || !(parent.revents & POLLIN))
			return EXIT_FAILURE;
	}
	if (!restore_vt(tty, control, vt, previous_vt))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
