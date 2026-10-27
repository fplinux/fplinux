// SPDX-License-Identifier: GPL-2.0-only
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Keep reports observable when a test replaces its child's environment. */

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#define REPORT_OPTIONS \
	"log_path=" FPLINUX_SANITIZER_REPORT_PATH ":log_exe_name=1:symbolize=1"

const char *__asan_default_options(void);
const char *__ubsan_default_options(void);
void __ubsan_on_report(void);

const char *__asan_default_options(void)
{
	return REPORT_OPTIONS ":detect_leaks=0";
}

const char *__ubsan_default_options(void)
{
	return REPORT_OPTIONS ":halt_on_error=1:print_stacktrace=1";
}

/* The fatal UB report must survive a caller that expects a nonzero exit. */
void __ubsan_on_report(void)
{
	char path[PATH_MAX];
	long descriptor;

	if (snprintf(path, sizeof(path), "%s.ubsan.%ld",
		     FPLINUX_SANITIZER_REPORT_PATH,
		     (long)getpid()) >= (int)sizeof(path))
		abort();
	/* Diagnostic I/O must bypass the subject's --wrap=open/close doubles. */
	descriptor = syscall(SYS_openat, AT_FDCWD, path,
			     O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (descriptor < 0)
		abort();
	if (descriptor != STDERR_FILENO) {
		if (syscall(SYS_dup3, descriptor, STDERR_FILENO, 0) < 0)
			abort();
		syscall(SYS_close, descriptor);
	}
}
