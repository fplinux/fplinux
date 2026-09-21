// SPDX-License-Identifier: GPL-2.0-only
#include "fplinux-cli.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum token_kind {
	TOKEN_END,
	TOKEN_OPTION,
	TOKEN_HELP,
	TOKEN_UNKNOWN,
	TOKEN_AMBIGUOUS,
	TOKEN_MISSING_VALUE,
	TOKEN_UNEXPECTED_VALUE,
	TOKEN_POSITIONAL,
	TOKEN_DUPLICATE,
	TOKEN_EMPTY_VALUE,
};

struct token {
	enum token_kind kind;
	size_t option;
	const char *argument;
	const char *value;
};

struct scanner {
	const struct fplinux_cli *cli;
	int argc;
	char **argv;
	int index;
	size_t positional;
	const char *short_options;
	const char *short_argument;
	bool options_ended;
	char **tail;
};

static struct token find_long_option(const struct fplinux_cli *cli,
				     const char *argument, size_t length)
{
	const char *name = argument + 2;
	struct token token = { .kind = TOKEN_UNKNOWN, .argument = argument };
	size_t matches = 0;
	size_t i;

	if (!length)
		return token;
	if (length == 4 && !strncmp(name, "help", length)) {
		token.kind = TOKEN_HELP;
		return token;
	}
	for (i = 0; i < cli->option_count; ++i) {
		const char *candidate = cli->options[i].name;

		if (!candidate || strncmp(candidate, name, length))
			continue;
		token.kind = TOKEN_OPTION;
		token.option = i;
		if (strlen(candidate) == length)
			return token;
		++matches;
	}
	if (length < 4 && !strncmp("help", name, length)) {
		token.kind = TOKEN_HELP;
		++matches;
	}
	if (matches > 1)
		token.kind = TOKEN_AMBIGUOUS;
	return token;
}

static struct token next_token(struct scanner *scanner)
{
	for (;;) {
		const struct fplinux_cli *cli = scanner->cli;
		const char *argument;
		struct token token;

		if (scanner->short_options && *scanner->short_options) {
			char letter = *scanner->short_options++;

			return (struct token){
				.kind = letter == 'h' ? TOKEN_HELP :
							TOKEN_UNKNOWN,
				.argument = scanner->short_argument,
			};
		}
		if (scanner->index == scanner->argc)
			return (struct token){ .kind = TOKEN_END };
		argument = scanner->argv[scanner->index++];
		if (!scanner->options_ended && !strcmp(argument, "--")) {
			scanner->options_ended = true;
			if (cli->tail_usage) {
				scanner->tail = &scanner->argv[scanner->index];
				return (struct token){ .kind = TOKEN_END };
			}
			continue;
		}
		if (!scanner->options_ended && argument[0] == '-' &&
		    argument[1]) {
			const char *equal;
			bool takes_value;

			if (argument[1] != '-') {
				scanner->short_argument = argument;
				scanner->short_options = argument + 1;
				continue;
			}
			equal = strchr(argument + 2, '=');
			token = find_long_option(
				cli, argument,
				equal ? (size_t)(equal - argument - 2) :
					strlen(argument + 2));
			if (token.kind != TOKEN_OPTION &&
			    token.kind != TOKEN_HELP)
				return token;
			takes_value = token.kind == TOKEN_OPTION &&
				      cli->options[token.option].metavar;
			if (!takes_value) {
				if (equal)
					token.kind = TOKEN_UNEXPECTED_VALUE;
				return token;
			}
			if (equal)
				token.value = equal + 1;
			else if (scanner->index < scanner->argc)
				token.value = scanner->argv[scanner->index++];
			else
				token.kind = TOKEN_MISSING_VALUE;
			return token;
		}
		while (scanner->positional < cli->option_count &&
		       cli->options[scanner->positional].name)
			++scanner->positional;
		if (scanner->positional == cli->option_count)
			return (struct token){ .kind = TOKEN_POSITIONAL,
					       .argument = argument };
		token = (struct token){
			.kind = TOKEN_OPTION,
			.option = scanner->positional++,
			.argument = argument,
			.value = argument,
		};
		return token;
	}
}

static void print_command(FILE *stream, const struct fplinux_cli *cli)
{
	fputs(cli->program, stream);
	if (cli->command)
		fprintf(stream, " %s", cli->command);
}

static void print_option(FILE *stream, const struct fplinux_cli_option *option)
{
	if (option->name) {
		fprintf(stream, "--%s", option->name);
		if (option->metavar)
			fprintf(stream, "=%s", option->metavar);
	} else {
		fputs(option->metavar, stream);
	}
}

void fplinux_cli_usage(FILE *stream, const struct fplinux_cli *cli)
{
	size_t i;

	print_command(stream, cli);
	fputs(" [-h]", stream);
	for (i = 0; i < cli->option_count; ++i) {
		const struct fplinux_cli_option *option = &cli->options[i];
		bool required = option->flags & FPLINUX_CLI_REQUIRED;

		fputc(' ', stream);
		if (!required)
			fputc('[', stream);
		print_option(stream, option);
		if (!required)
			fputc(']', stream);
		if (option->flags & FPLINUX_CLI_REPEAT)
			fputs("...", stream);
	}
	if (cli->tail_usage)
		fprintf(stream, " -- %s", cli->tail_usage);
}

static void print_help(const struct fplinux_cli *cli)
{
	size_t i;

	fputs("Usage: ", stdout);
	fplinux_cli_usage(stdout, cli);
	printf("\n\n%s\n\nOptions:\n", cli->description);
	printf("  -h, --help\n      Show this help message and exit.\n");
	for (i = 0; i < cli->option_count; ++i) {
		const struct fplinux_cli_option *option = &cli->options[i];

		fputs("  ", stdout);
		print_option(stdout, option);
		printf("\n      %s\n", option->help);
	}
}

static void print_hint(const struct fplinux_cli *cli)
{
	fputs("Try '", stderr);
	print_command(stderr, cli);
	fputs(" --help' for more information.\n", stderr);
}

enum fplinux_cli_result fplinux_cli_error(const struct fplinux_cli *cli,
					  const char *message)
{
	print_command(stderr, cli);
	fprintf(stderr, ": %s\n", message);
	print_hint(cli);
	return FPLINUX_CLI_ERROR;
}

static enum fplinux_cli_result syntax_error(const struct fplinux_cli *cli,
					    struct token token)
{
	print_command(stderr, cli);
	fputs(": ", stderr);
	switch (token.kind) {
	case TOKEN_UNKNOWN:
		fprintf(stderr, "unrecognized option '%s'", token.argument);
		break;
	case TOKEN_AMBIGUOUS:
		fprintf(stderr, "ambiguous option '%s'", token.argument);
		break;
	case TOKEN_MISSING_VALUE:
		fprintf(stderr, "option '%s' requires a value", token.argument);
		break;
	case TOKEN_UNEXPECTED_VALUE:
		fprintf(stderr, "option '%s' does not take a value",
			token.argument);
		break;
	case TOKEN_POSITIONAL:
		fprintf(stderr, "unexpected argument '%s'", token.argument);
		break;
	case TOKEN_DUPLICATE:
		print_option(stderr, &cli->options[token.option]);
		fputs(" may only be given once", stderr);
		break;
	case TOKEN_EMPTY_VALUE:
		print_option(stderr, &cli->options[token.option]);
		fputs(" requires a nonempty value", stderr);
		break;
	default:
		fputs("invalid arguments", stderr);
		break;
	}
	fputc('\n', stderr);
	print_hint(cli);
	return FPLINUX_CLI_ERROR;
}

enum fplinux_cli_result fplinux_cli_parse(struct fplinux_cli *cli, int argc,
					  char **argv)
{
	struct scanner scanner = {
		.cli = cli, .argc = argc, .argv = argv, .index = 1
	};
	struct token error = { .kind = TOKEN_END };
	struct token token;
	bool help = false;
	size_t i;

	cli->tail_argv = NULL;
	for (i = 0; i < cli->option_count; ++i) {
		cli->options[i].count = 0;
		cli->options[i].value = NULL;
	}
	while ((token = next_token(&scanner)).kind != TOKEN_END) {
		if (token.kind == TOKEN_HELP) {
			help = true;
			continue;
		}
		if (token.kind == TOKEN_OPTION) {
			struct fplinux_cli_option *option =
				&cli->options[token.option];

			++option->count;
			option->value = token.value;
			if (option->count > 1 &&
			    !(option->flags & FPLINUX_CLI_REPEAT))
				token.kind = TOKEN_DUPLICATE;
			else if (option->metavar && !token.value[0])
				token.kind = TOKEN_EMPTY_VALUE;
		}
		if (token.kind != TOKEN_OPTION && error.kind == TOKEN_END)
			error = token;
	}
	cli->tail_argv = scanner.tail;
	if (help) {
		print_help(cli);
		return FPLINUX_CLI_HELP;
	}
	if (error.kind != TOKEN_END)
		return syntax_error(cli, error);
	for (i = 0; i < cli->option_count; ++i) {
		const struct fplinux_cli_option *option = &cli->options[i];

		if ((option->flags & FPLINUX_CLI_REQUIRED) && !option->count) {
			print_command(stderr, cli);
			fputs(": missing required argument ", stderr);
			print_option(stderr, option);
			fputc('\n', stderr);
			print_hint(cli);
			return FPLINUX_CLI_ERROR;
		}
	}
	if (!cli->parse_option)
		return FPLINUX_CLI_READY;

	/* Reuse the scanner so help and syntax failures never invoke callbacks. */
	scanner = (struct scanner){
		.cli = cli, .argc = argc, .argv = argv, .index = 1
	};
	while ((token = next_token(&scanner)).kind != TOKEN_END) {
		const char *message =
			cli->parse_option(token.option, token.value, cli->data);

		if (message)
			return fplinux_cli_error(cli, message);
	}
	return FPLINUX_CLI_READY;
}

bool fplinux_cli_unsigned(const char *text, unsigned int minimum,
			  unsigned int maximum, unsigned int *value)
{
	const unsigned char *cursor = (const unsigned char *)text;
	char *end;
	unsigned long parsed;

	while (isspace(*cursor))
		++cursor;
	if (*cursor == '-')
		return false;
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || text[0] == '\0' || end[0] != '\0' || parsed < minimum ||
	    parsed > maximum)
		return false;
	*value = (unsigned int)parsed;
	return true;
}
