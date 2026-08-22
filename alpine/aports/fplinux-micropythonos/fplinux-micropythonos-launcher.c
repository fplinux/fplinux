// SPDX-License-Identifier: GPL-2.0-only
/* Run MicroPythonOS in a temporary FPLinux framebuffer session. */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fplinux-micropythonos-launcher-internal.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define FPLINUX_MICROPYTHONOS_LAUNCHER_FRAMEBUFFER_DEVICE "/dev/fb0"
#ifndef FPLINUX_MICROPYTHONOS_LAUNCHER_LOCK_PATH
#define FPLINUX_MICROPYTHONOS_LAUNCHER_LOCK_PATH \
	"/tmp/fplinux-micropythonos.lock"
#endif
#define FPLINUX_MICROPYTHONOS_LAUNCHER_TTY_DEVICE "/dev/tty0"

struct fplinux_micropythonos_display_state {
	int framebuffer;
	int tty;
	int tty_mode;
	int active_vt;
	uint8_t *mapping;
	uint8_t *backup;
	size_t memory_bytes;
	struct fb_var_screeninfo variable;
};

static volatile sig_atomic_t pending_signal;

static _Noreturn void fplinux_micropythonos_launcher_die(const char *message)
{
	fprintf(stderr, "micropythonos: %s\n", message);
	exit(EXIT_FAILURE);
}

static _Noreturn void
fplinux_micropythonos_launcher_die_errno(const char *message)
{
	fprintf(stderr, "micropythonos: %s: %s\n", message, strerror(errno));
	exit(EXIT_FAILURE);
}

bool fplinux_micropythonos_launcher_has_two_pages(
	const struct fb_var_screeninfo *variable)
{
	return variable->yres <= UINT32_MAX / 2U &&
	       variable->yres_virtual == variable->yres * 2U;
}

bool fplinux_micropythonos_launcher_framebuffer_valid(
	const struct fb_fix_screeninfo *fixed,
	const struct fb_var_screeninfo *variable)
{
	size_t page_bytes;
	bool has_two_pages;

	if (fixed->type != FB_TYPE_PACKED_PIXELS ||
	    fixed->visual != FB_VISUAL_TRUECOLOR || variable->xres == 0 ||
	    variable->yres == 0 || variable->xres_virtual != variable->xres ||
	    variable->xoffset != 0 || variable->bits_per_pixel != 16 ||
	    variable->red.offset != 11 || variable->red.length != 5 ||
	    variable->red.msb_right != 0 || variable->green.offset != 5 ||
	    variable->green.length != 6 || variable->green.msb_right != 0 ||
	    variable->blue.offset != 0 || variable->blue.length != 5 ||
	    variable->blue.msb_right != 0 || variable->transp.length != 0 ||
	    variable->xres > UINT32_MAX / sizeof(uint16_t) ||
	    fixed->line_length < variable->xres * sizeof(uint16_t) ||
	    fixed->line_length > UINT32_MAX / variable->yres)
		return false;

	has_two_pages = fplinux_micropythonos_launcher_has_two_pages(variable);
	if (variable->yres_virtual != variable->yres && !has_two_pages)
		return false;
	if ((!has_two_pages && variable->yoffset != 0) ||
	    (has_two_pages && variable->yoffset != 0 &&
	     variable->yoffset != variable->yres))
		return false;

	page_bytes = (size_t)fixed->line_length * variable->yres;
	if (fixed->smem_len < page_bytes)
		return false;
	if (has_two_pages &&
	    (fixed->ypanstep == 0 || page_bytes > UINT32_MAX / 2U ||
	     fixed->smem_len < page_bytes * 2U))
		return false;
	return true;
}

int fplinux_micropythonos_launcher_acquire_session_lock(void)
{
	const struct flock lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
	};
	int descriptor;

	descriptor = open(FPLINUX_MICROPYTHONOS_LAUNCHER_LOCK_PATH,
			  O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (descriptor < 0)
		fplinux_micropythonos_launcher_die_errno(
			"cannot create session lock");
	if (fcntl(descriptor, F_SETLK, &lock) < 0) {
		int saved_errno = errno;

		if (errno == EACCES || errno == EAGAIN) {
			close(descriptor);
			fplinux_micropythonos_launcher_die(
				"another MicroPythonOS session is already running");
		}
		close(descriptor);
		errno = saved_errno;
		fplinux_micropythonos_launcher_die_errno("cannot lock session");
	}
	return descriptor;
}

static void fplinux_micropythonos_launcher_save_display(
	struct fplinux_micropythonos_display_state *state)
{
	struct fb_fix_screeninfo fixed;
	struct vt_stat vt;

	memset(state, 0, sizeof(*state));
	state->framebuffer = -1;
	state->tty = open(FPLINUX_MICROPYTHONOS_LAUNCHER_TTY_DEVICE,
			  O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (state->tty < 0)
		fplinux_micropythonos_launcher_die_errno(
			"cannot open /dev/tty0");
	if (ioctl(state->tty, KDGETMODE, &state->tty_mode) < 0 ||
	    ioctl(state->tty, VT_GETSTATE, &vt) < 0)
		fplinux_micropythonos_launcher_die_errno(
			"cannot read console state");
	if (state->tty_mode != KD_TEXT)
		fplinux_micropythonos_launcher_die(
			"active console is not in text mode");
	state->active_vt = vt.v_active;

	state->framebuffer =
		open(FPLINUX_MICROPYTHONOS_LAUNCHER_FRAMEBUFFER_DEVICE,
		     O_RDWR | O_CLOEXEC);
	if (state->framebuffer < 0)
		fplinux_micropythonos_launcher_die_errno(
			"cannot open /dev/fb0");
	if (ioctl(state->framebuffer, FBIOGET_FSCREENINFO, &fixed) < 0 ||
	    ioctl(state->framebuffer, FBIOGET_VSCREENINFO, &state->variable) <
		    0)
		fplinux_micropythonos_launcher_die_errno(
			"cannot read framebuffer state");
	if (!fplinux_micropythonos_launcher_framebuffer_valid(&fixed,
							      &state->variable))
		fplinux_micropythonos_launcher_die(
			"unexpected framebuffer ABI");
	state->memory_bytes = fixed.smem_len;
	state->mapping = mmap(NULL, state->memory_bytes, PROT_READ | PROT_WRITE,
			      MAP_SHARED, state->framebuffer, 0);
	if (state->mapping == MAP_FAILED) {
		state->mapping = NULL;
		fplinux_micropythonos_launcher_die_errno(
			"cannot map framebuffer");
	}
	state->backup = malloc(state->memory_bytes);
	if (!state->backup)
		fplinux_micropythonos_launcher_die(
			"cannot allocate framebuffer backup");
	memcpy(state->backup, state->mapping, state->memory_bytes);
}

static bool fplinux_micropythonos_launcher_restore_display(
	struct fplinux_micropythonos_display_state *state)
{
	struct fb_var_screeninfo pan;
	bool restored = true;

	memcpy(state->mapping, state->backup, state->memory_bytes);
	__sync_synchronize();

	state->variable.activate = FB_ACTIVATE_NOW;
	if (ioctl(state->framebuffer, FBIOPUT_VSCREENINFO, &state->variable) <
	    0) {
		fprintf(stderr,
			"micropythonos: cannot restore framebuffer geometry: %s\n",
			strerror(errno));
		restored = false;
	} else if (fplinux_micropythonos_launcher_has_two_pages(
			   &state->variable)) {
		pan = state->variable;
		pan.xoffset = 0;
		pan.yoffset = state->variable.yoffset;
		pan.activate = FB_ACTIVATE_NOW;
		if (ioctl(state->framebuffer, FBIOPAN_DISPLAY, &pan) < 0) {
			fprintf(stderr,
				"micropythonos: cannot restore framebuffer page: %s\n",
				strerror(errno));
			restored = false;
		}
	}
	if (ioctl(state->tty, KDSETMODE, state->tty_mode) < 0) {
		fprintf(stderr,
			"micropythonos: cannot restore console mode: %s\n",
			strerror(errno));
		restored = false;
	}
	if (ioctl(state->tty, VT_ACTIVATE, state->active_vt) < 0 ||
	    ioctl(state->tty, VT_WAITACTIVE, state->active_vt) < 0) {
		fprintf(stderr,
			"micropythonos: cannot reactivate console VT: %s\n",
			strerror(errno));
		restored = false;
	}
	return restored;
}

static void fplinux_micropythonos_launcher_close_display(
	struct fplinux_micropythonos_display_state *state)
{
	free(state->backup);
	if (state->mapping)
		munmap(state->mapping, state->memory_bytes);
	if (state->framebuffer >= 0)
		close(state->framebuffer);
	if (state->tty >= 0)
		close(state->tty);
}

static void fplinux_micropythonos_launcher_catch_signal(int signal_number)
{
	pending_signal = signal_number;
}

static void fplinux_micropythonos_launcher_install_signal_handlers(void)
{
	static const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	struct sigaction action = {
		.sa_handler = fplinux_micropythonos_launcher_catch_signal,
	};
	size_t index;

	sigemptyset(&action.sa_mask);
	for (index = 0; index < ARRAY_SIZE(signals); ++index)
		if (sigaction(signals[index], &action, NULL) < 0)
			fplinux_micropythonos_launcher_die_errno(
				"cannot install signal handler");
}

static void fplinux_micropythonos_launcher_reset_signal_handlers(void)
{
	static const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	struct sigaction action = {
		.sa_handler = SIG_DFL,
	};
	size_t index;

	sigemptyset(&action.sa_mask);
	for (index = 0; index < ARRAY_SIZE(signals); ++index)
		if (sigaction(signals[index], &action, NULL) < 0)
			_exit(126);
}

pid_t fplinux_micropythonos_launcher_start_command(char *const command[])
{
	pid_t child = fork();

	if (child < 0)
		return -1;
	if (child == 0) {
		fplinux_micropythonos_launcher_reset_signal_handlers();
		execvp(command[0], command);
		fprintf(stderr, "micropythonos: cannot execute %s: %s\n",
			command[0], strerror(errno));
		_exit(126);
	}
	return child;
}

int fplinux_micropythonos_launcher_wait_for_command(pid_t child)
{
	int status;

	for (;;) {
		if (pending_signal) {
			int signal_number = pending_signal;

			pending_signal = 0;
			if (kill(child, signal_number) < 0 && errno != ESRCH)
				fprintf(stderr,
					"micropythonos: cannot forward signal: %s\n",
					strerror(errno));
		}
		if (waitpid(child, &status, 0) == child)
			break;
		if (errno != EINTR) {
			fprintf(stderr,
				"micropythonos: cannot wait for command: %s\n",
				strerror(errno));
			return EXIT_FAILURE;
		}
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	struct fplinux_micropythonos_display_state display;
	pid_t child;
	int command_status;
	int lock;
	bool restored;

	if (argc < 2) {
		fprintf(stderr,
			"usage: micropythonos <command> [argument ...]\n");
		return EXIT_FAILURE;
	}
	lock = fplinux_micropythonos_launcher_acquire_session_lock();
	fplinux_micropythonos_launcher_save_display(&display);
	fplinux_micropythonos_launcher_install_signal_handlers();
	if (ioctl(display.tty, KDSETMODE, KD_GRAPHICS) < 0)
		fplinux_micropythonos_launcher_die_errno(
			"cannot set console graphics mode");
	child = fplinux_micropythonos_launcher_start_command(argv + 1);
	if (child < 0) {
		fprintf(stderr, "micropythonos: cannot start command: %s\n",
			strerror(errno));
		command_status = EXIT_FAILURE;
	} else {
		command_status =
			fplinux_micropythonos_launcher_wait_for_command(child);
	}
	restored = fplinux_micropythonos_launcher_restore_display(&display);
	fplinux_micropythonos_launcher_close_display(&display);
	close(lock);
	return restored ? command_status : EXIT_FAILURE;
}
