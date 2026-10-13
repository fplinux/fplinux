/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_LOG_H
#define FPLINUX_TEST_SDBOOT_LOG_H

/* Host admission tests use the no-CONFIG_LOG error-output boundary. */
#include <stdio.h>

#define log_err(...) printf(__VA_ARGS__)

#endif
