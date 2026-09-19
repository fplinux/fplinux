/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-cli.h"

#include <stdio.h>
#include <string.h>

enum option_index { INPUT, MODE, DISPLAY, DISPLAY_MS, OPERAND };

struct command_options {
	unsigned int hold_ms;
	size_t callbacks;
};

/* Controlled command validation: no device, process or filesystem effects. */
static const char *parse_option(size_t option, const char *value, void *data)
{
	struct command_options *options = data;

	++options->callbacks;
	switch (option) {
	case MODE:
		if (strcmp(value, "fast") && strcmp(value, "slow"))
			return "mode must be fast or slow";
		break;
	case DISPLAY:
		options->hold_ms = 2000;
		break;
	case DISPLAY_MS:
		if (!fplinux_cli_unsigned(value, 0, 60000, &options->hold_ms))
			return "display-ms must be in 0..60000";
		break;
	default:
		break;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct fplinux_cli_option arguments[] = {
		[INPUT] = { .name = "input",
			    .metavar = "PATH",
			    .help = "Input file.",
			    .flags = FPLINUX_CLI_REQUIRED |
				     FPLINUX_CLI_REPEAT },
		[MODE] = { .name = "mode",
			   .metavar = "fast|slow",
			   .help = "Mode." },
		[DISPLAY] = { .name = "display",
			      .help = "Display with a default hold.",
			      .flags = FPLINUX_CLI_REPEAT },
		[DISPLAY_MS] = { .name = "display-ms",
				 .metavar = "MS",
				 .help = "Display with an explicit hold.",
				 .flags = FPLINUX_CLI_REPEAT },
		[OPERAND] = { .metavar = "OPERAND",
			      .help = "Optional operand." },
	};
	struct command_options options = { .hold_ms = 77 };
	struct fplinux_cli cli = {
		.program = "cli-probe",
		.description =
			"Exercise command-line behavior without external resources.",
		.options = arguments,
		.option_count = sizeof(arguments) / sizeof(arguments[0]),
		.parse_option = parse_option,
		.data = &options,
	};
	enum fplinux_cli_result result;
	int i;

	if (argc < 2)
		return 125;
	if (!strcmp(argv[1], "tail")) {
		cli.option_count = OPERAND;
		cli.tail_usage = "command [argument ...]";
	}
	--argc;
	++argv;
	result = fplinux_cli_parse(&cli, argc, argv);
	if (result == FPLINUX_CLI_HELP)
		return options.callbacks ? 125 : 0;
	if (result == FPLINUX_CLI_ERROR) {
		fprintf(stderr, "fixture callbacks=%zu\n", options.callbacks);
		return 2;
	}
	printf("input=%s\ninput_count=%zu\nhold_ms=%u\noperand=%s\n",
	       arguments[INPUT].value, arguments[INPUT].count, options.hold_ms,
	       arguments[OPERAND].value ? arguments[OPERAND].value : "<none>");
	for (i = 1; i < argc; ++i)
		printf("argv[%d]=<%s>\n", i, argv[i]);
	if (cli.tail_argv) {
		printf("tail_index=%td\n", cli.tail_argv - argv);
		for (i = 0; cli.tail_argv[i]; ++i)
			printf("tail[%d]=<%s>\n", i, cli.tail_argv[i]);
	}
	return 0;
}
