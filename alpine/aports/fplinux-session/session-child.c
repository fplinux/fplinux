/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-child.h"

#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fplinux-desktop-entry.h"
#include "session-core.h"

/*
 * Switches the child to the application account. A manager that is not root
 * runs as that account already, which is checked instead.
 */
static bool use_app_account(const struct fplinux_session_options *options)
{
	if (geteuid() != 0)
		return getuid() == options->app_uid &&
		       getgid() == options->app_gid;
	return initgroups(options->user, options->app_gid) == 0 &&
	       setgid(options->app_gid) == 0 && setuid(options->app_uid) == 0;
}

/*
 * The session runs with a low OOM score (see fplinux-session) so that the
 * kernel kills applications first; they start with the default score. Raising
 * the score needs no privilege, but after the credential switch /proc/self
 * belongs to root until exec, so this runs first. Errors are ignored.
 */
static void reset_oom_score(void)
{
	int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (write(fd, "0\n", 2) < 0) {
			/* Ignored: the score stays low, which only protects the app. */
		}
		close(fd);
	}
}

/*
 * The kernel clears the parent-death signal on a credential change, so it is set after the
 * switch; a manager that died before that is detected through getppid().
 */
_Noreturn void
fplinux_session_child_start(const struct fplinux_session_options *options,
			    const struct fplinux_desktop_entry *entry,
			    const char *lang, const char *soundfont,
			    pid_t manager)
{
	const char *home = options->home;
	sigset_t signals;

	sigemptyset(&signals);
	sigprocmask(SIG_SETMASK, &signals, NULL);
	reset_oom_score();
	if (setpgid(0, 0) < 0 || !use_app_account(options) ||
	    prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != manager)
		_exit(126);
	umask(0077);
	setenv("HOME", home, 1);
	setenv("USER", options->user, 1);
	setenv("LOGNAME", options->user, 1);
	if (getenv("FPLINUX_APP_RUNTIME_DIR"))
		setenv("XDG_RUNTIME_DIR", getenv("FPLINUX_APP_RUNTIME_DIR"), 1);
	setenv("LANG", lang, 1);
	if (soundfont) {
		if (setenv("FPLINUX_MIDI_SOUNDFONT_PATH", soundfont, 1) < 0)
			_exit(126);
	} else {
		unsetenv("FPLINUX_MIDI_SOUNDFONT_PATH");
	}
	setenv("FPLINUX_UI_SESSION_SOCKET", options->socket_path, 1);
	setenv("FPLINUX_APP_ID", entry->app_id, 1);
	chdir(home);
	execl(entry->executable, entry->executable, (char *)NULL);
	_exit(127);
}
