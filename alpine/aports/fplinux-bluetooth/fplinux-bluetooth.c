// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fplinux-bluetooth-common.h"
#include "fplinux-bluetooth-opp.h"
#include "fplinux-bluetooth-pan.h"
#include "fplinux-cli.h"

#ifndef FPLINUX_BLUETOOTH_DRIVER_DIR
#define FPLINUX_BLUETOOTH_DRIVER_DIR \
	"/sys/bus/platform/drivers/ums9117-bluetooth"
#endif

static volatile sig_atomic_t interrupted;

static void on_signal(int unused)
{
	(void)unused;
	interrupted = 1;
}

static int command_enable(bool optional)
{
	DIR *directory = opendir(FPLINUX_BLUETOOTH_DRIVER_DIR);
	struct dirent *entry;
	char path[NAME_MAX + sizeof("/start")];
	int directory_fd;
	int start_fd = -1;
	int saved_error;
	ssize_t written;

	if (!directory) {
		if (errno == ENOENT && optional)
			return 0;
		return fplinux_bluetooth_fail(
			"CM4 Bluetooth is not configured for this target: %s",
			strerror(errno));
	}
	directory_fd = dirfd(directory);
	if (directory_fd < 0) {
		saved_error = errno;
		closedir(directory);
		return fplinux_bluetooth_fail(
			"cannot access CM4 control directory: %s",
			strerror(saved_error));
	}
	while ((entry = readdir(directory))) {
		int candidate;

		if (entry->d_name[0] == '.')
			continue;
		if (snprintf(path, sizeof(path), "%s/start", entry->d_name) >=
		    (int)sizeof(path))
			continue;
		candidate = openat(directory_fd, path, O_WRONLY | O_CLOEXEC);
		if (candidate < 0) {
			if (errno == ENOENT || errno == ENOTDIR)
				continue;
			saved_error = errno;
			if (start_fd >= 0)
				close(start_fd);
			closedir(directory);
			return fplinux_bluetooth_fail(
				"cannot open CM4 start control: %s",
				strerror(saved_error));
		}
		if (start_fd >= 0) {
			close(candidate);
			close(start_fd);
			closedir(directory);
			return fplinux_bluetooth_fail(
				"multiple CM4 Bluetooth controllers are present");
		}
		start_fd = candidate;
	}
	closedir(directory);
	if (start_fd < 0)
		return optional ?
			       0 :
			       fplinux_bluetooth_fail(
				       "CM4 Bluetooth is not configured for this target");
	written = write(start_fd, "1\n", 2);
	saved_error = errno;
	close(start_fd);
	if (written == 2) {
		puts("Bluetooth interfaces ready; use bluetoothctl for adapter power");
		return 0;
	}
	if (written >= 0)
		saved_error = EIO;
	if (saved_error == ENODATA) {
		const char *message =
			"Bluetooth firmware is absent; prepare it on the host with ./fplinux device-data prepare <target> and install the resulting system";

		if (optional) {
			fprintf(stderr, "fplinux-bluetooth: %s\n", message);
			return 0;
		}
		return fplinux_bluetooth_fail("%s", message);
	}
	if (saved_error == EINVAL || saved_error == EBADMSG ||
	    saved_error == EKEYREJECTED)
		return fplinux_bluetooth_fail(
			"Bluetooth firmware is incomplete or invalid; prepare and reinstall the target system");
	return fplinux_bluetooth_fail("cannot start CM4 Bluetooth: %s",
				      strerror(saved_error));
}

static bool valid_peer(const char *peer)
{
	if (strlen(peer) != 17)
		return false;
	for (size_t index = 0; index < 17; ++index) {
		if (index % 3 == 2) {
			if (peer[index] != ':')
				return false;
		} else if (!((peer[index] >= '0' && peer[index] <= '9') ||
			     (peer[index] >= 'A' && peer[index] <= 'F') ||
			     (peer[index] >= 'a' && peer[index] <= 'f')))
			return false;
	}
	return true;
}

enum command {
	COMMAND_ENABLE,
	COMMAND_SEND,
	COMMAND_RECEIVE,
	COMMAND_NETWORK,
	COMMAND_NONE,
};

struct options {
	enum command command;
	bool if_present;
	const char *peer;
	const char *path;
	int seconds;
};

static const char *parse_option(size_t option, const char *value, void *data)
{
	struct options *options = data;
	unsigned int seconds;

	if ((options->command == COMMAND_SEND ||
	     options->command == COMMAND_NETWORK ||
	     options->command == COMMAND_RECEIVE) &&
	    option == 0 && !valid_peer(value))
		return "peer must be a Bluetooth address such as 01:23:45:67:89:AB";
	if (options->command == COMMAND_RECEIVE && option == 2) {
		if (!fplinux_cli_unsigned(value, 1, 3600, &seconds))
			return "receive seconds must be between 1 and 3600";
		options->seconds = (int)seconds;
	}
	return NULL;
}

static int parse_arguments(int argc, char **argv, struct options *options)
{
	struct fplinux_cli_option root_options[] = {
		{
			.metavar = "COMMAND",
			.help = "enable, send, receive or network",
			.flags = FPLINUX_CLI_REQUIRED,
		},
	};
	struct fplinux_cli_option enable_options[] = {
		{
			.name = "if-present",
			.help = "permit an unconfigured board or absent firmware",
		},
	};
	struct fplinux_cli_option send_options[] = {
		{
			.metavar = "PEER",
			.help = "Bluetooth address XX:XX:XX:XX:XX:XX",
			.flags = FPLINUX_CLI_REQUIRED,
		},
		{
			.metavar = "FILE",
			.help = "file to send",
			.flags = FPLINUX_CLI_REQUIRED,
		},
	};
	struct fplinux_cli_option receive_options[] = {
		{
			.metavar = "PEER",
			.help = "Bluetooth address XX:XX:XX:XX:XX:XX",
			.flags = FPLINUX_CLI_REQUIRED,
		},
		{
			.metavar = "DIR",
			.help = "receive directory",
			.flags = FPLINUX_CLI_REQUIRED,
		},
		{
			.metavar = "SECONDS",
			.help = "receive timeout, from 1 to 3600",
			.flags = FPLINUX_CLI_REQUIRED,
		},
	};
	struct fplinux_cli_option network_options[] = {
		{
			.metavar = "PEER",
			.help = "Bluetooth address XX:XX:XX:XX:XX:XX",
			.flags = FPLINUX_CLI_REQUIRED,
		},
	};
	struct fplinux_cli commands[] = {
		[COMMAND_ENABLE] = {
			.command = "enable",
			.description =
				"Start the board's prepared CM4 firmware after root mount.",
			.options = enable_options,
			.option_count = sizeof(enable_options) /
					sizeof(enable_options[0]),
		},
		[COMMAND_SEND] = {
			.command = "send",
			.description = "Send one file to a paired peer.",
			.options = send_options,
			.option_count = sizeof(send_options) / sizeof(send_options[0]),
		},
		[COMMAND_RECEIVE] = {
			.command = "receive",
			.description = "Accept one file from a paired peer.",
			.options = receive_options,
			.option_count = sizeof(receive_options) /
					sizeof(receive_options[0]),
		},
		[COMMAND_NETWORK] = {
			.command = "network",
			.description = "Connect to a paired peer's NAP service.",
			.options = network_options,
			.option_count = sizeof(network_options) /
					sizeof(network_options[0]),
		},
	};
	struct fplinux_cli root_cli = {
		.program = argv[0],
		.description =
			"Use bluetoothctl for adapter power, discovery and pairing.",
		.options = root_options,
		.option_count = sizeof(root_options) / sizeof(root_options[0]),
	};
	struct fplinux_cli *selected = NULL;
	enum fplinux_cli_result result;
	enum command command;

	options->command = COMMAND_NONE;
	for (command = COMMAND_ENABLE; command < COMMAND_NONE; ++command) {
		if (argc > 1 && !strcmp(argv[1], commands[command].command)) {
			options->command = command;
			selected = &commands[command];
			break;
		}
	}
	if (options->command == COMMAND_NONE) {
		result = fplinux_cli_parse(&root_cli, argc, argv);
		if (result == FPLINUX_CLI_HELP) {
			fputs("\nCommands:\n", stdout);
			for (command = COMMAND_ENABLE; command < COMMAND_NONE;
			     ++command) {
				commands[command].program = argv[0];
				fputs("  ", stdout);
				fplinux_cli_usage(stdout, &commands[command]);
				fputc('\n', stdout);
			}
		}
		if (result != FPLINUX_CLI_READY)
			return result;
		return fplinux_cli_error(&root_cli, "unknown command");
	}
	selected->program = argv[0];
	selected->parse_option = parse_option;
	selected->data = options;
	result = fplinux_cli_parse(selected, argc - 1, argv + 1);
	if (result != FPLINUX_CLI_READY)
		return result;
	switch (options->command) {
	case COMMAND_ENABLE:
		options->if_present = selected->options[0].count != 0;
		break;
	case COMMAND_SEND:
		options->peer = selected->options[0].value;
		options->path = selected->options[1].value;
		break;
	case COMMAND_RECEIVE:
		options->peer = selected->options[0].value;
		options->path = selected->options[1].value;
		break;
	case COMMAND_NETWORK:
		options->peer = selected->options[0].value;
		break;
	case COMMAND_NONE:
		return FPLINUX_CLI_ERROR;
	}
	return FPLINUX_CLI_READY;
}

int main(int argc, char **argv)
{
	struct options options = { 0 };
	struct sigaction action = { .sa_handler = on_signal };
	int parse_result = parse_arguments(argc, argv, &options);

	if (parse_result != FPLINUX_CLI_READY)
		return parse_result;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0)
		return fplinux_bluetooth_fail(
			"cannot install signal handlers: %s", strerror(errno));
	switch (options.command) {
	case COMMAND_ENABLE:
		return command_enable(options.if_present);
	case COMMAND_SEND:
		return fplinux_bluetooth_opp_send(options.peer, options.path,
						  &interrupted);
	case COMMAND_RECEIVE:
		return fplinux_bluetooth_opp_receive(options.peer, options.path,
						     options.seconds,
						     &interrupted);
	case COMMAND_NETWORK:
		return fplinux_bluetooth_pan_connect(options.peer,
						     &interrupted);
	case COMMAND_NONE:
		return 2;
	}
	return 2;
}
