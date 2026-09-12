/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_ENV_H
#define FPLINUX_TEST_SDBOOT_ENV_H

int env_set(const char *name, const char *value);
int env_set_hex(const char *name, unsigned long value);

#endif
