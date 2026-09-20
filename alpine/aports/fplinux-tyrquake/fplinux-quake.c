// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fplinux-cli.h"
#include "fplinux-quake-internal.h"
#include "fplinux-fb-session.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define FPLINUX_QUAKE_CARD_MOUNT "/mnt/card"
#define FPLINUX_QUAKE_GAME_DATA FPLINUX_QUAKE_CARD_MOUNT "/fplinux/quake/id1"
#define FPLINUX_QUAKE_ENGINE "/usr/bin/tyr-quake"
#define FPLINUX_QUAKE_FRAMEBUFFER_DEVICE "/dev/fb0"
#define FPLINUX_QUAKE_LOCK_PATH "/tmp/fplinux-quake.lock"
#define FPLINUX_QUAKE_TTY_DEVICE "/dev/tty0"
/* Kibibytes the engine reserves for its own allocator. The default suits a
 * phone that keeps its game data on a card; a phone holding that data in RAM
 * has less room and can ask for less. Below the classic minimum the engine
 * refuses to start, so the range stops there. */
#define FPLINUX_QUAKE_HEAP_DEFAULT_KIB 32768UL
#define FPLINUX_QUAKE_HEAP_MINIMUM_KIB 8192UL
#define FPLINUX_QUAKE_HEAP_MAXIMUM_KIB 262144UL

static volatile sig_atomic_t pending_signal;

static _Noreturn void die(const char *message)
{
	fprintf(stderr, "quake: %s\n", message);
	exit(EXIT_FAILURE);
}

static _Noreturn void die_errno(const char *message)
{
	fprintf(stderr, "quake: %s: %s\n", message, strerror(errno));
	exit(EXIT_FAILURE);
}

static void catch_signal(int signal_number)
{
	pending_signal = signal_number;
}

static void install_signal_handlers(void)
{
	static const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	struct sigaction action = {
		.sa_handler = catch_signal,
	};
	size_t i;

	sigemptyset(&action.sa_mask);
	for (i = 0; i < ARRAY_SIZE(signals); ++i)
		if (sigaction(signals[i], &action, NULL) < 0)
			die_errno("cannot install signal handler");
}

static void reset_signal_handlers(void)
{
	static const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	struct sigaction action = {
		.sa_handler = SIG_DFL,
	};
	size_t i;

	sigemptyset(&action.sa_mask);
	for (i = 0; i < ARRAY_SIZE(signals); ++i)
		if (sigaction(signals[i], &action, NULL) < 0)
			_exit(126);
}

static int acquire_lock(void)
{
	struct flock claim = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
	};
	int descriptor = open(FPLINUX_QUAKE_LOCK_PATH,
			      O_RDWR | O_CREAT | O_CLOEXEC, 0600);

	if (descriptor < 0)
		die_errno("cannot open game-session lock");
	if (fcntl(descriptor, F_SETLK, &claim) < 0) {
		int saved_errno = errno;

		close(descriptor);
		if (saved_errno == EACCES || saved_errno == EAGAIN)
			die("another Quake session is running");
		errno = saved_errno;
		die_errno("cannot acquire game-session lock");
	}
	return descriptor;
}

static bool path_is_block_device(const char *path)
{
	struct stat status;

	return stat(path, &status) == 0 && S_ISBLK(status.st_mode);
}

static bool card_is_mounted(void)
{
	char line[1024];
	FILE *mountinfo = fopen("/proc/self/mountinfo", "r");

	if (!mountinfo)
		die_errno("cannot read mount table");
	while (fgets(line, sizeof(line), mountinfo)) {
		if (strstr(line, " " FPLINUX_QUAKE_CARD_MOUNT " ")) {
			fclose(mountinfo);
			return true;
		}
	}
	if (ferror(mountinfo)) {
		fclose(mountinfo);
		die_errno("cannot read mount table");
	}
	fclose(mountinfo);
	return false;
}

static void mount_card(void)
{
	const char *source;

	if (mkdir(FPLINUX_QUAKE_CARD_MOUNT, 0755) < 0 && errno != EEXIST)
		die_errno("cannot create " FPLINUX_QUAKE_CARD_MOUNT);
	if (card_is_mounted())
		return;

	if (path_is_block_device("/dev/mmcblk0p1"))
		source = "/dev/mmcblk0p1";
	else if (path_is_block_device("/dev/mmcblk0"))
		source = "/dev/mmcblk0";
	else
		die("microSD is unavailable; insert it before boot");

	if (mount(source, FPLINUX_QUAKE_CARD_MOUNT, "vfat",
		  MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC, "utf8=1") < 0)
		die_errno(
			"cannot mount microSD read-only at " FPLINUX_QUAKE_CARD_MOUNT);
}

static void require_pak(const char *path)
{
	struct stat status;

	if (stat(path, &status) < 0 || !S_ISREG(status.st_mode) ||
	    access(path, R_OK) < 0)
		die("game data is missing: " FPLINUX_QUAKE_GAME_DATA
		    "/pak0.pak");
}

static void write_phone_config(const char *directory)
{
	static const char config[] = "bind \"UPARROW\" \"+forward\"\n"
				     "bind \"DOWNARROW\" \"+back\"\n"
				     "bind \"LEFTARROW\" \"+left\"\n"
				     "bind \"RIGHTARROW\" \"+right\"\n"
				     "bind \"ENTER\" \"+attack\"\n"
				     "bind \"CTRL\" \"+attack\"\n"
				     "bind \"SPACE\" \"+jump\"\n"
				     "bind \"TAB\" \"+jump\"\n"
				     "bind \"1\" \"+moveleft\"\n"
				     "bind \"2\" \"+left\"\n"
				     "bind \"3\" \"+moveright\"\n"
				     "bind \"4\" \"+back\"\n"
				     "bind \"5\" \"+right\"\n"
				     "bind \"6\" \"+forward\"\n"
				     "bind \"7\" \"impulse 12\"\n"
				     "bind \"8\" \"+speed\"\n"
				     "bind \"9\" \"impulse 10\"\n";
	char path[256];
	ssize_t written;
	int descriptor;

	if (snprintf(path, sizeof(path), "%s/config.cfg", directory) >=
	    (int)sizeof(path))
		die("runtime path is too long");
	descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (descriptor < 0)
		die_errno("cannot create phone control configuration");
	written = write(descriptor, config, sizeof(config) - 1);
	if (written != (ssize_t)(sizeof(config) - 1)) {
		int saved_errno = errno;

		close(descriptor);
		errno = written < 0 ? saved_errno : EIO;
		die_errno("cannot write phone control configuration");
	}
	if (close(descriptor) < 0)
		die_errno("cannot close phone control configuration");
}

static void prepare_runtime(char *runtime, size_t runtime_size,
			    const char *input_mode)
{
	char id1[256];
	unsigned int index;
	bool found_pak0 = false;

	if (snprintf(runtime, runtime_size, "/tmp/fplinux-quake.%ld",
		     (long)getpid()) >= (int)runtime_size)
		die("runtime path is too long");
	if (snprintf(id1, sizeof(id1), "%s/id1", runtime) >= (int)sizeof(id1))
		die("runtime path is too long");
	if (mkdir(runtime, 0700) < 0 || mkdir(id1, 0700) < 0)
		die_errno("cannot create volatile Quake directory");

	for (index = 0; index < 10; ++index) {
		char source[256];
		char destination[256];
		struct stat status;

		if (snprintf(source, sizeof(source),
			     FPLINUX_QUAKE_GAME_DATA "/pak%u.pak",
			     index) >= (int)sizeof(source) ||
		    snprintf(destination, sizeof(destination), "%s/pak%u.pak",
			     id1, index) >= (int)sizeof(destination))
			die("game-data path is too long");
		if (stat(source, &status) < 0) {
			if (errno == ENOENT)
				continue;
			die_errno("cannot inspect game data");
		}
		if (!S_ISREG(status.st_mode) || access(source, R_OK) < 0)
			die("Quake PAK is not a readable regular file");
		if (symlink(source, destination) < 0)
			die_errno(
				"cannot link game data into volatile directory");
		if (index == 0)
			found_pak0 = true;
	}
	if (!found_pak0)
		die("game data is missing: " FPLINUX_QUAKE_GAME_DATA
		    "/pak0.pak");
	if (strcmp(input_mode, "phone") == 0)
		write_phone_config(id1);
}

static int remove_runtime_entry(const char *path, const struct stat *status,
				int type, struct FTW *tree)
{
	(void)status;
	(void)tree;
	return type == FTW_DP ? rmdir(path) : unlink(path);
}

void fplinux_quake_remove_runtime(const char *runtime)
{
	if (nftw(runtime, remove_runtime_entry, 8, FTW_DEPTH | FTW_PHYS) < 0 &&
	    errno != ENOENT)
		fprintf(stderr, "quake: cannot remove volatile runtime: %s\n",
			strerror(errno));
}

static void save_display(struct fplinux_fb_session *session)
{
	char error[128];

	if (!fplinux_fb_session_open(session, FPLINUX_QUAKE_FRAMEBUFFER_DEVICE,
				     FPLINUX_QUAKE_TTY_DEVICE, error,
				     sizeof(error)))
		die(error);
}

static pid_t start_engine(const char *runtime, const char *input_mode,
			  unsigned long heap_kib)
{
	char heap[32];
	pid_t child;

	if (snprintf(heap, sizeof(heap), "%lu", heap_kib) >= (int)sizeof(heap))
		die("heap size is too long");

	char *const arguments[] = {
		(char *)FPLINUX_QUAKE_ENGINE,
		"-nolan",
		"-basedir",
		(char *)runtime,
		"-heapsize",
		heap,
		"-input",
		(char *)input_mode,
		"+mlook",
		NULL,
	};

	child = fork();

	if (child < 0)
		die_errno("cannot start TyrQuake");
	if (child == 0) {
		reset_signal_handlers();
		if (setenv("HOME", runtime, 1) < 0)
			_exit(126);
		execv(FPLINUX_QUAKE_ENGINE, arguments);
		fprintf(stderr, "quake: cannot execute %s: %s\n",
			FPLINUX_QUAKE_ENGINE, strerror(errno));
		_exit(126);
	}
	return child;
}

static int wait_for_engine(pid_t child)
{
	int status;

	for (;;) {
		if (pending_signal) {
			int signal_number = pending_signal;

			pending_signal = 0;
			if (kill(child, signal_number) < 0 && errno != ESRCH)
				fprintf(stderr,
					"quake: cannot forward signal: %s\n",
					strerror(errno));
		}
		if (waitpid(child, &status, 0) == child)
			break;
		if (errno != EINTR)
			die_errno("cannot wait for TyrQuake");
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return EXIT_FAILURE;
}

enum quake_option {
	QUAKE_OPTION_INPUT,
	QUAKE_OPTION_HEAPSIZE,
};

struct quake_options {
	const char *input_mode;
	unsigned long heap_kib;
};

static bool parse_heap_kib(const char *value, unsigned long *heap_kib,
			   const char **error)
{
	static const char *const decimal_only =
		"--heapsize requires a decimal size in kibibytes";
	const char *cursor;
	unsigned long parsed;
	char *end;

	if (value[0] == '\0') {
		*error = decimal_only;
		return false;
	}
	for (cursor = value; *cursor; ++cursor)
		if (*cursor < '0' || *cursor > '9') {
			*error = decimal_only;
			return false;
		}
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno == ERANGE || end == value || *end != '\0' ||
	    parsed < FPLINUX_QUAKE_HEAP_MINIMUM_KIB ||
	    parsed > FPLINUX_QUAKE_HEAP_MAXIMUM_KIB) {
		*error = "--heapsize must be between 8192 and 262144 kibibytes";
		return false;
	}
	*heap_kib = parsed;
	return true;
}

static const char *parse_quake_option(size_t option, const char *value,
				      void *data)
{
	struct quake_options *options = data;
	const char *argument_error;

	switch (option) {
	case QUAKE_OPTION_INPUT:
		if (strcmp(value, "phone") && strcmp(value, "keyboard"))
			return "--input must be phone or keyboard";
		options->input_mode = value;
		return NULL;
	case QUAKE_OPTION_HEAPSIZE:
		if (!parse_heap_kib(value, &options->heap_kib, &argument_error))
			return argument_error;
		return NULL;
	default:
		return "unknown option";
	}
}

static enum fplinux_cli_result parse_arguments(int argc, char **argv,
					       struct quake_options *options)
{
	struct fplinux_cli_option entries[] = {
		[QUAKE_OPTION_INPUT] = {
			.name = "input",
			.metavar = "phone|keyboard",
			.help = "select the game controls",
			.flags = FPLINUX_CLI_REQUIRED,
		},
		[QUAKE_OPTION_HEAPSIZE] = {
			.name = "heapsize",
			.metavar = "KIBIBYTES",
			.help = "memory the engine reserves (default 32768)",
		},
	};
	struct fplinux_cli cli = {
		.program = argv[0],
		.description = "Run Quake with the selected controls.",
		.options = entries,
		.option_count = ARRAY_SIZE(entries),
		.parse_option = parse_quake_option,
		.data = options,
	};

	options->input_mode = NULL;
	options->heap_kib = FPLINUX_QUAKE_HEAP_DEFAULT_KIB;
	return fplinux_cli_parse(&cli, argc, argv);
}

int main(int argc, char **argv)
{
	struct fplinux_fb_session display;
	struct quake_options options;
	char pak0[256];
	char runtime[128];
	pid_t child;
	int child_status;
	int lock;
	enum fplinux_cli_result parse_result;
	bool restored;

	parse_result = parse_arguments(argc, argv, &options);
	if (parse_result != FPLINUX_CLI_READY)
		return parse_result;
	lock = acquire_lock();
	mount_card();
	if (snprintf(pak0, sizeof(pak0), FPLINUX_QUAKE_GAME_DATA "/pak0.pak") >=
	    (int)sizeof(pak0))
		die("game-data path is too long");
	require_pak(pak0);
	prepare_runtime(runtime, sizeof(runtime), options.input_mode);
	save_display(&display);
	install_signal_handlers();
	child = start_engine(runtime, options.input_mode, options.heap_kib);
	child_status = wait_for_engine(child);
	restored = fplinux_fb_session_close(&display);
	fplinux_quake_remove_runtime(runtime);
	close(lock);
	return restored ? child_status : EXIT_FAILURE;
}
