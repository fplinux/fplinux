/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_COMMAND_H
#define FPLINUX_TEST_SDBOOT_COMMAND_H

struct cmd_tbl {
	int (*command)(struct cmd_tbl *, int, int, char *const[]);
};

/* Register the normal command entry for the host harness. */
#define U_BOOT_CMD(name, maxargs, repeatable, function, usage, help) \
	struct cmd_tbl test_command = { .command = function }
#define CMD_RET_FAILURE 1

#endif
