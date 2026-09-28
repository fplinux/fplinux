// SPDX-License-Identifier: GPL-2.0-only
#include "fplinux-brightness-client.h"
#include "fplinux-cli.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

enum { OPTION_SOCKET, OPTION_ACTION, OPTION_LEVEL };

static const char *parse_option(size_t option, const char *value, void *data)
{
	(void)option;
	(void)value;
	(void)data;
	return NULL;
}

int main(int argc, char **argv)
{
	struct fplinux_cli_option options[] = {
		[OPTION_SOCKET] = { .name = "socket",
				    .metavar = "PATH",
				    .help = "Use a selected brightness service socket." },
		[OPTION_ACTION] = { .metavar = "{get|set}",
				    .help = "Read or set the desired logical brightness.",
				    .flags = FPLINUX_CLI_REQUIRED },
		[OPTION_LEVEL] = { .metavar = "LEVEL",
				   .help = "Level from 0 to 10." },
	};
	struct fplinux_cli cli = {
		.program = "fplinux-brightness",
		.description = "Read or set the display's logical brightness.",
		.options = options,
		.option_count = sizeof(options) / sizeof(options[0]),
		.parse_option = parse_option,
	};
	struct fplinux_brightness_client client = { .fd = -1 };
	unsigned int level = 0;
	enum fplinux_cli_result parsed;
	int result;

	parsed = fplinux_cli_parse(&cli, argc, argv);
	if (parsed != FPLINUX_CLI_READY)
		return parsed;
	if (!strcmp(options[OPTION_ACTION].value, "get")) {
		if (options[OPTION_LEVEL].count)
			return fplinux_cli_error(&cli, "get takes no level");
	} else if (!strcmp(options[OPTION_ACTION].value, "set")) {
		if (!options[OPTION_LEVEL].count ||
		    !fplinux_cli_unsigned(options[OPTION_LEVEL].value, 0, 10,
					  &level) ||
		    (options[OPTION_LEVEL].value[0] == '0' &&
		     options[OPTION_LEVEL].value[1]) ||
		    (options[OPTION_LEVEL].value[0] < '0' ||
		     options[OPTION_LEVEL].value[0] > '9'))
			return fplinux_cli_error(
				&cli, "set requires a level from 0 to 10");
	} else {
		return fplinux_cli_error(&cli, "action must be get or set");
	}
	if (fplinux_brightness_connect(&client, options[OPTION_SOCKET].value) <
	    0) {
		fprintf(stderr,
			"fplinux-brightness: brightness service unavailable: %s\n",
			strerror(errno));
		return 1;
	}
	if (!strcmp(options[OPTION_ACTION].value, "get"))
		result = fplinux_brightness_get(&client, &level);
	else
		result = fplinux_brightness_set(&client, level);
	if (result < 0)
		fprintf(stderr, "fplinux-brightness: %s failed: %s\n",
			options[OPTION_ACTION].value, strerror(errno));
	else if (!strcmp(options[OPTION_ACTION].value, "get"))
		printf("%u\n", level);
	fplinux_brightness_close(&client);
	return result < 0 ? 1 : 0;
}
