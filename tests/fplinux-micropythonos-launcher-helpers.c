/* SPDX-License-Identifier: GPL-2.0-only */
/* Host harness for isolated launcher helpers; it does not access a framebuffer or VT. */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../alpine/aports/fplinux-micropythonos/fplinux-micropythonos-launcher-internal.h"

static int test_framebuffer_helpers(void)
{
	struct fb_fix_screeninfo fixed = {
		.type = FB_TYPE_PACKED_PIXELS,
		.visual = FB_VISUAL_TRUECOLOR,
		.line_length = 256,
		.smem_len = 256 * 160,
	};
	struct fb_var_screeninfo variable = {
		.xres = 128,
		.yres = 160,
		.xres_virtual = 128,
		.yres_virtual = 160,
		.bits_per_pixel = 16,
		.red = { .offset = 11, .length = 5 },
		.green = { .offset = 5, .length = 6 },
		.blue = { .offset = 0, .length = 5 },
	};

	if (!fplinux_micropythonos_launcher_framebuffer_valid(&fixed,
							      &variable) ||
	    fplinux_micropythonos_launcher_has_two_pages(&variable))
		return EXIT_FAILURE;

	variable.yres_virtual = 320;
	variable.yoffset = 160;
	fixed.ypanstep = 1;
	fixed.smem_len *= 2;
	if (!fplinux_micropythonos_launcher_framebuffer_valid(&fixed,
							      &variable) ||
	    !fplinux_micropythonos_launcher_has_two_pages(&variable))
		return EXIT_FAILURE;

	variable.yres_virtual = 480;
	fixed.smem_len *= 3;
	if (fplinux_micropythonos_launcher_framebuffer_valid(&fixed, &variable))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

static int test_command_helpers(void)
{
	char *const command[] = {
		"/bin/sh",
		"-c",
		"test \"$1\" = \"argument with spaces\"; exit 37",
		"sh",
		"argument with spaces",
		NULL,
	};
	pid_t child = fplinux_micropythonos_launcher_start_command(command);

	if (child < 0)
		return EXIT_FAILURE;
	if (fplinux_micropythonos_launcher_wait_for_command(child) != 37)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

static int test_session_lock_helper(void)
{
	int lock = fplinux_micropythonos_launcher_acquire_session_lock();
	pid_t child = fork();
	int status;

	if (child < 0) {
		close(lock);
		return EXIT_FAILURE;
	}
	if (child == 0) {
		fplinux_micropythonos_launcher_acquire_session_lock();
		_exit(2);
	}
	if (waitpid(child, &status, 0) != child) {
		close(lock);
		return EXIT_FAILURE;
	}
	close(lock);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_FAILURE)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return EXIT_FAILURE;
	if (strcmp(argv[1], "framebuffer") == 0)
		return test_framebuffer_helpers();
	if (strcmp(argv[1], "command") == 0)
		return test_command_helpers();
	if (strcmp(argv[1], "lock") == 0)
		return test_session_lock_helper();
	return EXIT_FAILURE;
}
