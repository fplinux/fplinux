// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "fplinux-cli.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FPLINUX_KMSG_RECORD_BYTES 1024U
#define FPLINUX_KMSG_TAG_BYTES 64U

struct log_options {
	unsigned int priority;
	const char *tag;
};

static const char *parse_option(size_t option, const char *value, void *data)
{
	struct log_options *options = data;

	if (option == 0) {
		if (!fplinux_cli_unsigned(value, 0, 7, &options->priority))
			return "--level must be between 0 and 7";
	} else {
		if (strlen(value) > FPLINUX_KMSG_TAG_BYTES ||
		    strpbrk(value, "\r\n"))
			return "--tag must be one line of at most 64 bytes";
		options->tag = value;
	}
	return NULL;
}

static int forward_lines(const struct log_options *options, int output)
{
	char record[FPLINUX_KMSG_RECORD_BYTES];
	char line[FPLINUX_KMSG_RECORD_BYTES];
	size_t tag_length = strlen(options->tag);
	size_t prefix = (size_t)snprintf(record, sizeof(record),
					 "<%u>%s: ", options->priority,
					 options->tag);
	bool first_fragment = true;

	/* Reserve a newline for fragments; each write is one kernel record. */
	while (fgets(line, (int)(sizeof(record) - prefix - 1), stdin)) {
		const char *message = line;
		size_t length = strlen(line);
		bool complete = length && line[length - 1] == '\n';
		ssize_t written;

		if (complete)
			line[--length] = '\0';
		if (length && line[length - 1] == '\r')
			line[--length] = '\0';
		if (first_fragment && length > tag_length &&
		    !strncmp(line, options->tag, tag_length) &&
		    line[tag_length] == ':') {
			message += tag_length + 1;
			if (*message == ' ')
				++message;
		}
		first_fragment = complete;
		length = strlen(message);
		if (!length)
			continue;
		memcpy(record + prefix, message, length);
		record[prefix + length] = '\n';
		length += prefix + 1;
		do {
			written = write(output, record, length);
		} while (written < 0 && errno == EINTR);
		if (written != (ssize_t)length) {
			if (written >= 0)
				errno = EIO;
			perror("fplinux-kmsg: cannot write a kernel log record");
			return 1;
		}
	}
	if (ferror(stdin)) {
		perror("fplinux-kmsg: cannot read daemon output");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct log_options options = {};
	struct fplinux_cli_option arguments[] = {
		{ .name = "level",
		  .metavar = "N",
		  .help = "kernel severity from 0 to 7",
		  .flags = FPLINUX_CLI_REQUIRED },
		{ .name = "tag",
		  .metavar = "NAME",
		  .help = "component name",
		  .flags = FPLINUX_CLI_REQUIRED },
	};
	struct fplinux_cli cli = {
		.program = "fplinux-kmsg",
		.description = "Forward daemon output to the kernel log",
		.options = arguments,
		.option_count = sizeof(arguments) / sizeof(arguments[0]),
		.parse_option = parse_option,
		.data = &options,
	};
	int result = fplinux_cli_parse(&cli, argc, argv);
	int output;

	if (result != FPLINUX_CLI_READY)
		return result;
	output = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (output < 0) {
		perror("fplinux-kmsg: cannot open /dev/kmsg");
		return 1;
	}
	result = forward_lines(&options, output);
	close(output);
	return result;
}
