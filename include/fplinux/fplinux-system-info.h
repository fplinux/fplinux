/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SYSTEM_INFO_H
#define FPLINUX_SYSTEM_INFO_H

#include <stdbool.h>
#include <stddef.h>

/* Read-only system facts; false when the source cannot supply the value. */
bool fplinux_system_info_read_os_field(const char *path, const char *field,
				       char *value, size_t size);
bool fplinux_system_info_read_os_name(const char *path, char *name,
				      size_t size);
bool fplinux_system_info_read_line(const char *path, char *line, size_t size);

#endif
