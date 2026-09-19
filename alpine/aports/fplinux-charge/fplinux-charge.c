// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fplinux-cli.h"

#ifndef FPLINUX_CHARGE_COUNTER_PATH
#define FPLINUX_CHARGE_COUNTER_PATH ""
#endif

#ifndef FPLINUX_CHARGE_COUNTER_GLOB
#define FPLINUX_CHARGE_COUNTER_GLOB "/sys/class/power_supply/*/charge_counter"
#endif

#define FPLINUX_CHARGE_INTERNAL_ERROR 125
#define FPLINUX_CHARGE_EXEC_NOENT 127
#define FPLINUX_CHARGE_EXEC_ERROR 126
#define FPLINUX_CHARGE_TEXT_BYTES 64
#define FPLINUX_CHARGE_COUNTER_SUFFIX "/charge_counter"

static volatile sig_atomic_t child_pid = -1;
static volatile sig_atomic_t pending_signal;

static const int forwarded_signals[] = {
	SIGHUP,
	SIGINT,
	SIGQUIT,
	SIGTERM,
};

static void handle_signal(int signal_number)
{
	int saved_errno = errno;
	pid_t pid;

	pending_signal = signal_number;
	pid = (pid_t)child_pid;
	if (pid > 0)
		(void)kill(pid, signal_number);
	errno = saved_errno;
}

static int set_signal_action(int signal_number, void (*handler)(int))
{
	struct sigaction action = {
		.sa_handler = handler,
	};

	sigemptyset(&action.sa_mask);
	return sigaction(signal_number, &action, NULL);
}

static int install_signal_handlers(void)
{
	for (size_t index = 0;
	     index < sizeof(forwarded_signals) / sizeof(forwarded_signals[0]);
	     index++)
		if (set_signal_action(forwarded_signals[index], handle_signal))
			return -1;

	return 0;
}

static void restore_default_signal_handlers(void)
{
	for (size_t index = 0;
	     index < sizeof(forwarded_signals) / sizeof(forwarded_signals[0]);
	     index++)
		(void)set_signal_action(forwarded_signals[index], SIG_DFL);
}

static int read_charge_counter(const char *path, long long *value)
{
	char text[FPLINUX_CHARGE_TEXT_BYTES];
	char *end;
	ssize_t length;
	int descriptor;
	long long parsed;

	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	length = read(descriptor, text, sizeof(text) - 1);
	if (length < 0)
		goto fail;
	text[length] = '\0';
	if (close(descriptor))
		return -1;
	errno = 0;
	parsed = strtoll(text, &end, 10);
	if (errno || end == text)
		return -1;
	while (isspace((unsigned char)*end))
		end++;
	if (*end) {
		errno = EINVAL;
		return -1;
	}
	*value = parsed;
	return 0;

fail: {
	int saved_errno = errno;

	(void)close(descriptor);
	errno = saved_errno;
}
	return -1;
}

static int run_command(char *const command[], int *status)
{
	pid_t result;
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		restore_default_signal_handlers();
		execvp(command[0], command);
		fprintf(stderr, "fplinux-charge: cannot execute %s: %s\n",
			command[0], strerror(errno));
		_exit(errno == ENOENT ? FPLINUX_CHARGE_EXEC_NOENT :
					FPLINUX_CHARGE_EXEC_ERROR);
	}

	child_pid = pid;
	if (pending_signal)
		(void)kill(pid, pending_signal);
	do {
		result = waitpid(pid, status, 0);
	} while (result < 0 && errno == EINTR);
	child_pid = -1;
	return result < 0 ? -1 : 0;
}

static double elapsed_seconds(const struct timespec *start,
			      const struct timespec *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

static int command_result(int status)
{
	int signal_number = 0;

	if (WIFSIGNALED(status))
		signal_number = WTERMSIG(status);
	else if (pending_signal)
		signal_number = pending_signal;
	if (signal_number) {
		(void)set_signal_action(signal_number, SIG_DFL);
		(void)raise(signal_number);
		_exit(128 + signal_number);
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);

	return FPLINUX_CHARGE_INTERNAL_ERROR;
}

static bool command_succeeded(int status)
{
	return WIFEXITED(status) && !WEXITSTATUS(status) && !pending_signal;
}

static bool read_power_supply_type(const char *counter_path)
{
	static const char suffix[] = FPLINUX_CHARGE_COUNTER_SUFFIX;
	char text[FPLINUX_CHARGE_TEXT_BYTES];
	char *type_path;
	ssize_t length;
	size_t base_length;
	size_t path_length;
	int descriptor;

	path_length = strlen(counter_path);
	if (path_length < sizeof(suffix) - 1U ||
	    strcmp(counter_path + path_length - sizeof(suffix) + 1U, suffix)) {
		errno = EINVAL;
		return false;
	}
	base_length = path_length - (sizeof(suffix) - 1U);
	type_path = malloc(base_length + sizeof("/type"));
	if (!type_path)
		return false;
	memcpy(type_path, counter_path, base_length);
	type_path[base_length] = '\0';
	strcat(type_path, "/type");
	descriptor = open(type_path, O_RDONLY | O_CLOEXEC);
	free(type_path);
	if (descriptor < 0)
		return false;
	length = read(descriptor, text, sizeof(text) - 1U);
	if (close(descriptor) < 0 && length >= 0)
		length = -1;
	if (length < 0)
		return false;
	text[length] = '\0';
	return !strcmp(text, "Battery\n") || !strcmp(text, "Battery");
}

static int select_charge_counter(char **selected, const char **error)
{
	glob_t matches = { 0 };
	const char *candidate = NULL;
	int result;
	size_t index;

	*selected = NULL;
	*error = NULL;
	if (FPLINUX_CHARGE_COUNTER_PATH[0] != '\0') {
		*selected = strdup(FPLINUX_CHARGE_COUNTER_PATH);
		return *selected ? 0 : -1;
	}
	result = glob(FPLINUX_CHARGE_COUNTER_GLOB, 0, NULL, &matches);
	if (result == GLOB_NOMATCH) {
		*error = "no Battery charge counter is available";
		errno = ENODEV;
		return -1;
	}
	if (result != 0) {
		errno = EIO;
		return -1;
	}
	for (index = 0; index < matches.gl_pathc; ++index) {
		if (!read_power_supply_type(matches.gl_pathv[index]))
			continue;
		if (candidate) {
			*error =
				"multiple Battery charge counters are available";
			errno = ENOTUNIQ;
			goto fail;
		}
		candidate = matches.gl_pathv[index];
	}
	if (!candidate) {
		*error = "no Battery charge counter is available";
		errno = ENODEV;
		goto fail;
	}
	*selected = strdup(candidate);
	if (!*selected)
		goto fail;
	globfree(&matches);
	return 0;

fail:
	globfree(&matches);
	return -1;
}

static enum fplinux_cli_result parse_arguments(int argc, char **argv,
					       const char **counter_path,
					       char ***command)
{
	struct fplinux_cli_option options[] = {
		{
			.name = "counter",
			.metavar = "PATH",
			.help = "read this counter instead of discovering a Battery",
		},
	};
	struct fplinux_cli cli = {
		.program = argv[0],
		.description = "Measure battery charge while a command runs.",
		.options = options,
		.option_count = sizeof(options) / sizeof(options[0]),
		.tail_usage = "command [argument ...]",
	};
	enum fplinux_cli_result result;

	*counter_path = NULL;
	*command = NULL;
	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	if (!cli.tail_argv || !cli.tail_argv[0])
		return fplinux_cli_error(&cli,
					 "command must follow a -- separator");
	if (!cli.tail_argv[0][0])
		return fplinux_cli_error(&cli, "command must not be empty");
	*counter_path = options[0].value;
	*command = cli.tail_argv;
	return FPLINUX_CLI_READY;
}

int main(int argc, char **argv)
{
	struct timespec start;
	struct timespec end;
	long long charge_before;
	long long charge_after;
	long long charge_delta;
	char **command;
	char *selected_counter = NULL;
	const char *counter_error;
	const char *counter_path;
	double average_current;
	double elapsed;
	int status;
	int result;

	result = parse_arguments(argc, argv, &counter_path, &command);
	if (result != FPLINUX_CLI_READY)
		return result;
	if (counter_path) {
		selected_counter = strdup(counter_path);
		if (!selected_counter) {
			perror("fplinux-charge: cannot select charge counter");
			return FPLINUX_CHARGE_INTERNAL_ERROR;
		}
	} else if (select_charge_counter(&selected_counter, &counter_error)) {
		if (counter_error)
			fprintf(stderr,
				"fplinux-charge: cannot select charge counter: %s: %s\n",
				counter_error, strerror(errno));
		else
			fprintf(stderr,
				"fplinux-charge: cannot select charge counter: %s\n",
				strerror(errno));
		return FPLINUX_CHARGE_INTERNAL_ERROR;
	}
	if (read_charge_counter(selected_counter, &charge_before)) {
		fprintf(stderr, "fplinux-charge: cannot read %s: %s\n",
			selected_counter, strerror(errno));
		free(selected_counter);
		return FPLINUX_CHARGE_INTERNAL_ERROR;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &start)) {
		perror("fplinux-charge: clock_gettime");
		free(selected_counter);
		return FPLINUX_CHARGE_INTERNAL_ERROR;
	}
	if (install_signal_handlers()) {
		perror("fplinux-charge: sigaction");
		free(selected_counter);
		return FPLINUX_CHARGE_INTERNAL_ERROR;
	}
	if (run_command(command, &status)) {
		perror("fplinux-charge: waitpid");
		free(selected_counter);
		return FPLINUX_CHARGE_INTERNAL_ERROR;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &end) ||
	    read_charge_counter(selected_counter, &charge_after)) {
		fprintf(stderr,
			"fplinux-charge: command finished, but the final measurement failed: %s\n",
			strerror(errno));
		result = command_succeeded(status) ?
				 FPLINUX_CHARGE_INTERNAL_ERROR :
				 command_result(status);
		free(selected_counter);
		return result;
	}
	elapsed = elapsed_seconds(&start, &end);
	if (elapsed <= 0.0) {
		fprintf(stderr,
			"fplinux-charge: command finished, but monotonic time did not advance\n");
		result = command_succeeded(status) ?
				 FPLINUX_CHARGE_INTERNAL_ERROR :
				 command_result(status);
		free(selected_counter);
		return result;
	}
	charge_delta = charge_after - charge_before;
	average_current = (double)charge_delta * 3600.0 / elapsed;
	fprintf(stderr,
		"fplinux-charge: elapsed=%.3f s charge_delta=%+lld uAh average_current=%+.0f uA\n",
		elapsed, charge_delta, average_current);
	fflush(stderr);
	result = command_result(status);
	free(selected_counter);
	return result;
}
