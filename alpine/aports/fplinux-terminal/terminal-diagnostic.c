/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "terminal-diagnostic.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <unistd.h>

void fplinux_terminal_diagnostic_close(
	struct fplinux_terminal_diagnostic *diagnostic)
{
	while (diagnostic->count) {
		int fd = diagnostic->fds[--diagnostic->count];

		if (fd >= 0)
			close(fd);
	}
}

static bool show_return_hint(void)
{
	static const char hint[] =
		"\r\nDiagnostic console\r\nRight soft: return to terminal\r\n";
	size_t written = 0;
	int fd = open("/dev/tty1", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	int saved_errno;

	if (fd < 0)
		return false;
	while (written < sizeof(hint) - 1) {
		ssize_t size =
			write(fd, hint + written, sizeof(hint) - 1 - written);

		if (size > 0) {
			written += (size_t)size;
			continue;
		}
		if (size < 0 && errno == EINTR)
			continue;
		break;
	}
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return written == sizeof(hint) - 1;
}

bool fplinux_terminal_diagnostic_begin(
	struct fplinux_terminal_diagnostic *diagnostic,
	const struct fplinux_input_session *input, int control)
{
	int saved_errno;

	fplinux_terminal_diagnostic_close(diagnostic);
	for (size_t index = 0; index < FPLINUX_INPUT_SESSION_DEVICE_COUNT;
	     ++index) {
		const struct fplinux_input_session_device *device =
			&input->devices[index];
		int fd;

		if (device->fd < 0 ||
		    device->source != FPLINUX_INPUT_SOURCE_KEYPAD)
			continue;
		/* Suspend releases the grab and closes libinput's original fd. */
		fd = fcntl(device->fd, F_DUPFD_CLOEXEC, 0);
		if (fd < 0)
			goto fail;
		diagnostic->fds[diagnostic->count] = fd;
		diagnostic->right_pressed[diagnostic->count++] = false;
	}
	if (!show_return_hint() || ioctl(control, VT_ACTIVATE, 1) < 0)
		goto fail;
	return true;
fail:
	saved_errno = errno;
	fplinux_terminal_diagnostic_close(diagnostic);
	errno = saved_errno;
	return false;
}

bool fplinux_terminal_diagnostic_dispatch(
	struct fplinux_terminal_diagnostic *diagnostic, int control,
	int terminal_vt)
{
	for (size_t index = 0; index < diagnostic->count; ++index) {
		int fd = diagnostic->fds[index];
		struct input_event event;
		ssize_t size;

		if (fd < 0)
			continue;
		for (;;) {
			struct vt_stat state;

			size = read(fd, &event, sizeof(event));
			if (size < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (size == 0)
				break;
			if (size != sizeof(event)) {
				errno = EIO;
				return false;
			}
			if (event.type != EV_KEY || event.code != KEY_F14)
				continue;
			if (event.value == 1) {
				diagnostic->right_pressed[index] = true;
				continue;
			}
			/* Return on release so resume cannot replay a held soft key. */
			if (event.value != 0 ||
			    !diagnostic->right_pressed[index])
				continue;
			diagnostic->right_pressed[index] = false;
			if (ioctl(control, VT_GETSTATE, &state) < 0)
				return false;
			if (state.v_active != 1)
				continue;
			if (ioctl(control, VT_ACTIVATE, terminal_vt) < 0)
				return false;
			fplinux_terminal_diagnostic_close(diagnostic);
			return true;
		}
		if (size == 0 || errno == ENODEV) {
			close(fd);
			diagnostic->fds[index] = -1;
		} else if (errno != EAGAIN) {
			return false;
		}
	}
	return true;
}
