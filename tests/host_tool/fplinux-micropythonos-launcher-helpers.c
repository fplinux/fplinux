/* SPDX-License-Identifier: GPL-2.0-only */
/* Host harness for isolated launcher helpers; it does not access a framebuffer or VT. */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../alpine/aports/fplinux-micropythonos/fplinux-micropythonos-launcher-internal.h"

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
	if (strcmp(argv[1], "command") == 0)
		return test_command_helpers();
	if (strcmp(argv[1], "lock") == 0)
		return test_session_lock_helper();
	return EXIT_FAILURE;
}
