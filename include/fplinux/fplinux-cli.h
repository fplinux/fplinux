/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CLI_H
#define FPLINUX_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum fplinux_cli_option_flags {
	FPLINUX_CLI_REQUIRED = 1U << 0,
	/* Permit repeated occurrences of a named option. */
	FPLINUX_CLI_REPEAT = 1U << 1,
};

/* A null name declares a positional; a null metavar declares a named flag. */
struct fplinux_cli_option {
	const char *name;
	const char *metavar;
	const char *help;
	unsigned int flags;

	/* Borrowed last argv value and occurrence count, reset by each parse. */
	const char *value;
	size_t count;
};

enum fplinux_cli_result {
	FPLINUX_CLI_READY = -1,
	FPLINUX_CLI_HELP = 0,
	FPLINUX_CLI_ERROR = 2,
};

struct fplinux_cli {
	const char *program;
	const char *command;
	const char *description;
	struct fplinux_cli_option *options;
	size_t option_count;

	/*
	 * Validate and assign one occurrence, in argv order, after syntax and
	 * help have been resolved. Flags have a null value. Return a lasting
	 * error message or NULL. Resource access belongs after READY.
	 */
	const char *(*parse_option)(size_t option, const char *value,
				    void *data);
	void *data;

	/* Opt in to an opaque, unmodified argv suffix after a real -- token. */
	const char *tail_usage;
	char **tail_argv;
};

/*
 * Parse without allocating memory or modifying argv. -h/--help is built in;
 * recognized help takes precedence over argument errors. Long options accept
 * separate values, =values and unique prefixes. Values must be nonempty;
 * opaque tail arguments are not interpreted. Command defaults stay caller-owned.
 */
enum fplinux_cli_result fplinux_cli_parse(struct fplinux_cli *cli, int argc,
					  char **argv);

/* A synopsis without a newline, also usable in a command's subcommand list. */
void fplinux_cli_usage(FILE *stream, const struct fplinux_cli *cli);

/* Report a command-owned value or combination error with the shared help hint. */
enum fplinux_cli_result fplinux_cli_error(const struct fplinux_cli *cli,
					  const char *message);

/* Base-10 strtoul syntax without a minus; leave the output on failure. */
bool fplinux_cli_unsigned(const char *text, unsigned int minimum,
			  unsigned int maximum, unsigned int *value);

#endif
