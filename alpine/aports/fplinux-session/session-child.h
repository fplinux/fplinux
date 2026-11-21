/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_CHILD_H
#define FPLINUX_SESSION_CHILD_H

#include <sys/types.h>

struct fplinux_desktop_entry;
struct fplinux_session_options;

_Noreturn __attribute__((visibility("hidden"))) void
fplinux_session_child_start(const struct fplinux_session_options *options,
			    const struct fplinux_desktop_entry *entry,
			    const char *lang, const char *soundfont,
			    pid_t manager);

#endif
