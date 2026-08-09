// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define CARD_MOUNT "/mnt/card"
#define GAME_DATA CARD_MOUNT "/fplinux/quake/id1"
#define ENGINE "/usr/bin/tyr-quake"
#define FRAMEBUFFER "/dev/fb0"
#define FRAMEBUFFER_ID "ta1618-rgb565"
#define FRAMEBUFFER_BYTES 307200U
#define FRAMEBUFFER_HEIGHT 320U
#define FRAMEBUFFER_STRIDE 480U
#define FRAMEBUFFER_WIDTH 240U
#define LOCK_PATH "/tmp/fplinux-quake.lock"
#define TTY "/dev/tty0"

struct display_state {
	int framebuffer;
	int tty;
	int tty_mode;
	int active_vt;
	uint8_t *mapping;
	uint8_t *backup;
	size_t size;
	struct fb_var_screeninfo variable;
};

static volatile sig_atomic_t pending_signal;

static void die(const char *message)
{
	fprintf(stderr, "quake: %s\n", message);
	exit(EXIT_FAILURE);
}

static void die_errno(const char *message)
{
	fprintf(stderr, "quake: %s: %s\n", message, strerror(errno));
	exit(EXIT_FAILURE);
}

static void catch_signal(int signal_number) { pending_signal = signal_number; }

static void install_signal_handlers(void)
{
	static const int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM};
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
	static const int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM};
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
	int descriptor = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

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
		if (strstr(line, " " CARD_MOUNT " ")) {
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

	if (mkdir(CARD_MOUNT, 0755) < 0 && errno != EEXIST)
		die_errno("cannot create " CARD_MOUNT);
	if (card_is_mounted())
		return;

	if (path_is_block_device("/dev/mmcblk0p1"))
		source = "/dev/mmcblk0p1";
	else if (path_is_block_device("/dev/mmcblk0"))
		source = "/dev/mmcblk0";
	else
		die("microSD is unavailable; insert it before boot");

	if (mount(source, CARD_MOUNT, "vfat",
		  MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC, "utf8=1") < 0)
		die_errno("cannot mount microSD read-only at " CARD_MOUNT);
}

static void require_pak(const char *path)
{
	struct stat status;

	if (stat(path, &status) < 0 || !S_ISREG(status.st_mode) ||
	    access(path, R_OK) < 0)
		die("game data is missing: " GAME_DATA "/pak0.pak");
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

		if (snprintf(source, sizeof(source), GAME_DATA "/pak%u.pak",
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
		die("game data is missing: " GAME_DATA "/pak0.pak");
	if (strcmp(input_mode, "phone") == 0)
		write_phone_config(id1);
}

static void remove_runtime(const char *runtime, const char *input_mode)
{
	char id1[256];
	unsigned int index;

	if (snprintf(id1, sizeof(id1), "%s/id1", runtime) >= (int)sizeof(id1))
		return;
	for (index = 0; index < 10; ++index) {
		char path[256];

		if (snprintf(path, sizeof(path), "%s/pak%u.pak", id1, index) <
		    (int)sizeof(path))
			unlink(path);
	}
	if (strcmp(input_mode, "phone") == 0) {
		char path[256];

		if (snprintf(path, sizeof(path), "%s/config.cfg", id1) <
		    (int)sizeof(path))
			unlink(path);
	}
	rmdir(id1);
	rmdir(runtime);
}

static void validate_framebuffer(const struct fb_fix_screeninfo *fixed,
				 const struct fb_var_screeninfo *variable)
{
	if (strncmp(fixed->id, FRAMEBUFFER_ID, sizeof(fixed->id)) != 0 ||
	    fixed->type != FB_TYPE_PACKED_PIXELS ||
	    fixed->visual != FB_VISUAL_TRUECOLOR ||
	    fixed->line_length != FRAMEBUFFER_STRIDE ||
	    fixed->smem_len != FRAMEBUFFER_BYTES ||
	    fixed->ypanstep != FRAMEBUFFER_HEIGHT ||
	    variable->xres != FRAMEBUFFER_WIDTH ||
	    variable->yres != FRAMEBUFFER_HEIGHT ||
	    variable->xres_virtual != FRAMEBUFFER_WIDTH ||
	    (variable->yres_virtual != FRAMEBUFFER_HEIGHT &&
	     variable->yres_virtual != FRAMEBUFFER_HEIGHT * 2) ||
	    variable->xoffset != 0 ||
	    (variable->yoffset != 0 &&
	     variable->yoffset != FRAMEBUFFER_HEIGHT) ||
	    variable->bits_per_pixel != 16)
		die("unexpected framebuffer ABI");
}

static void save_display(struct display_state *state)
{
	struct fb_fix_screeninfo fixed;
	struct vt_stat vt;

	memset(state, 0, sizeof(*state));
	state->framebuffer = -1;
	state->tty = open(TTY, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (state->tty < 0)
		die_errno("cannot open " TTY);
	if (ioctl(state->tty, KDGETMODE, &state->tty_mode) < 0 ||
	    ioctl(state->tty, VT_GETSTATE, &vt) < 0)
		die_errno("cannot read console state");
	if (state->tty_mode != KD_TEXT)
		die("active console is not in text mode");
	state->active_vt = vt.v_active;

	state->framebuffer = open(FRAMEBUFFER, O_RDWR | O_CLOEXEC);
	if (state->framebuffer < 0)
		die_errno("cannot open " FRAMEBUFFER);
	if (ioctl(state->framebuffer, FBIOGET_FSCREENINFO, &fixed) < 0 ||
	    ioctl(state->framebuffer, FBIOGET_VSCREENINFO, &state->variable) <
		0)
		die_errno("cannot read framebuffer state");
	validate_framebuffer(&fixed, &state->variable);
	state->size = fixed.smem_len;
	state->mapping = mmap(NULL, state->size, PROT_READ | PROT_WRITE,
			      MAP_SHARED, state->framebuffer, 0);
	if (state->mapping == MAP_FAILED) {
		state->mapping = NULL;
		die_errno("cannot map framebuffer");
	}
	state->backup = malloc(state->size);
	if (!state->backup)
		die("cannot allocate framebuffer backup");
	memcpy(state->backup, state->mapping, state->size);
}

static bool restore_display(struct display_state *state)
{
	struct fb_var_screeninfo current;
	bool restored = true;

	memcpy(state->mapping, state->backup, state->size);
	__sync_synchronize();
	if (ioctl(state->framebuffer, FBIOGET_VSCREENINFO, &current) < 0) {
		fprintf(stderr,
			"quake: cannot read framebuffer during restore: %s\n",
			strerror(errno));
		restored = false;
	} else {
		current.xoffset = 0;
		current.yoffset = state->variable.yoffset;
		current.activate = FB_ACTIVATE_NOW;
		if (current.yoffset + FRAMEBUFFER_HEIGHT <=
			current.yres_virtual &&
		    ioctl(state->framebuffer, FBIOPAN_DISPLAY, &current) < 0) {
			fprintf(stderr,
				"quake: cannot restore framebuffer page: %s\n",
				strerror(errno));
			restored = false;
		}
	}
	if (ioctl(state->tty, KDSETMODE, state->tty_mode) < 0) {
		fprintf(stderr, "quake: cannot restore console mode: %s\n",
			strerror(errno));
		restored = false;
	}
	state->variable.activate = FB_ACTIVATE_NOW;
	if (ioctl(state->framebuffer, FBIOPUT_VSCREENINFO, &state->variable) <
	    0) {
		fprintf(stderr,
			"quake: cannot restore framebuffer geometry: %s\n",
			strerror(errno));
		restored = false;
	}
	if (ioctl(state->tty, VT_ACTIVATE, state->active_vt) < 0 ||
	    ioctl(state->tty, VT_WAITACTIVE, state->active_vt) < 0) {
		fprintf(stderr, "quake: cannot reactivate console VT: %s\n",
			strerror(errno));
		restored = false;
	}
	return restored;
}

static void close_display(struct display_state *state)
{
	free(state->backup);
	if (state->mapping)
		munmap(state->mapping, state->size);
	if (state->framebuffer >= 0)
		close(state->framebuffer);
	if (state->tty >= 0)
		close(state->tty);
}

static pid_t start_engine(const char *runtime, const char *input_mode)
{
	char *const arguments[] = {
	    (char *)ENGINE, "-nolan", "-basedir", (char *)runtime,
	    "-heapsize",    "32768",  "-input",	  (char *)input_mode,
	    "+mlook",	    NULL,
	};
	pid_t child = fork();

	if (child < 0)
		die_errno("cannot start TyrQuake");
	if (child == 0) {
		reset_signal_handlers();
		if (setenv("HOME", runtime, 1) < 0)
			_exit(126);
		execv(ENGINE, arguments);
		fprintf(stderr, "quake: cannot execute %s: %s\n", ENGINE,
			strerror(errno));
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

int main(int argc, char **argv)
{
	struct display_state display;
	char pak0[256];
	char runtime[128];
	const char *input_mode;
	pid_t child;
	int child_status;
	int lock;
	bool restored;

	if (argc != 3 || strcmp(argv[1], "--input") != 0 ||
	    (strcmp(argv[2], "phone") != 0 &&
	     strcmp(argv[2], "keyboard") != 0)) {
		fprintf(stderr, "usage: quake --input phone|keyboard\n");
		return EXIT_FAILURE;
	}
	input_mode = argv[2];
	lock = acquire_lock();
	mount_card();
	if (snprintf(pak0, sizeof(pak0), GAME_DATA "/pak0.pak") >=
	    (int)sizeof(pak0))
		die("game-data path is too long");
	require_pak(pak0);
	prepare_runtime(runtime, sizeof(runtime), input_mode);
	save_display(&display);
	install_signal_handlers();
	child = start_engine(runtime, input_mode);
	child_status = wait_for_engine(child);
	restored = restore_display(&display);
	close_display(&display);
	remove_runtime(runtime, input_mode);
	close(lock);
	return restored ? child_status : EXIT_FAILURE;
}
