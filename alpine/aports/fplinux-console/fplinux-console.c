/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * FPLinux local terminal for a Linux VT and normalized evdev keypad.
 *
 * Hardware-independent multi-tap behaviour is adapted from WiPhone GUI.h:
 * repeated presses cycle a character table and a timeout accepts it.
 * No ESP32/WiPhone keypad driver code is used.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "fplinux-multitap.h"

#define FPLINUX_CONSOLE_STAR_HOLD_MS 400
#define FPLINUX_CONSOLE_VISUAL_BELL_MS 250
#define FPLINUX_CONSOLE_KEYPAD_REOPEN_MS 1000
#define FPLINUX_CONSOLE_INPUT_DIRECTORY_ENTRY_LIMIT 1024
#define FPLINUX_CONSOLE_KEYPAD_EVENT_COUNT 8
#define FPLINUX_CONSOLE_KEYPAD_EVENT_MAX_BYTES 5
#define FPLINUX_CONSOLE_TERMINAL_INPUT_BYTES 64
#define FPLINUX_CONSOLE_TERMINAL_REPLY_BYTES 32
#define FPLINUX_CONSOLE_TERMINAL_REPLY_HOLD_MS 40
#define FPLINUX_CONSOLE_COMPOSED_CHARACTER_MAX_BYTES 2
/* A 64-byte read can complete a 31-byte buffered terminal reply. */
#define FPLINUX_CONSOLE_TERMINAL_REPLY_MAX_ENQUEUE_BYTES \
	(FPLINUX_CONSOLE_TERMINAL_INPUT_BYTES +          \
	 FPLINUX_CONSOLE_TERMINAL_REPLY_BYTES - 1)
#define FPLINUX_CONSOLE_TERMINAL_INPUT_FIFO_RESERVE_BYTES   \
	(FPLINUX_CONSOLE_TERMINAL_REPLY_MAX_ENQUEUE_BYTES + \
	 FPLINUX_CONSOLE_COMPOSED_CHARACTER_MAX_BYTES)
#define FPLINUX_CONSOLE_KEYPAD_INPUT_FIFO_RESERVE_BYTES   \
	(FPLINUX_CONSOLE_KEYPAD_EVENT_COUNT *             \
		 FPLINUX_CONSOLE_KEYPAD_EVENT_MAX_BYTES + \
	 FPLINUX_CONSOLE_COMPOSED_CHARACTER_MAX_BYTES)
#define FPLINUX_CONSOLE_PTY_TX_BYTES 4096
#define FPLINUX_CONSOLE_PTY_DRAIN_CHUNK_BYTES 512
#define FPLINUX_CONSOLE_PTY_DRAIN_READ_BUDGET 4
#define FPLINUX_CONSOLE_PTY_DRAIN_BUDGET_BYTES   \
	(FPLINUX_CONSOLE_PTY_DRAIN_CHUNK_BYTES * \
	 FPLINUX_CONSOLE_PTY_DRAIN_READ_BUDGET)
#define FPLINUX_CONSOLE_VCSA_DEVICE_MAJOR 7U
#define FPLINUX_CONSOLE_VCSA_DEVICE_MINOR_BASE 128U
#define FPLINUX_CONSOLE_CHILD_EXIT_GRACE_MS 1000
#define FPLINUX_CONSOLE_CHILD_EXIT_POLL_MS 20
#define FPLINUX_CONSOLE_VT_MAX_ROWS 128
#define FPLINUX_CONSOLE_VT_MAX_COLS 240
#define FPLINUX_CONSOLE_VT_MAX_CELLS \
	(FPLINUX_CONSOLE_VT_MAX_ROWS * FPLINUX_CONSOLE_VT_MAX_COLS)
#define FPLINUX_CONSOLE_TRANSCRIPT_LINE_COUNT 256
#define FPLINUX_CONSOLE_TRANSCRIPT_LINE_BYTES FPLINUX_CONSOLE_VT_MAX_COLS
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BIT_WORD(bit) ((bit) / BITS_PER_LONG)
#define BIT_MASK(bit) (1UL << ((bit) % BITS_PER_LONG))
#define BIT_ARRAY_SIZE(max_bit) (BIT_WORD(max_bit) + 1)
#define FPLINUX_CONSOLE_KEY_STATE_BYTES ((KEY_CNT + 7U) / 8U)

_Static_assert(FPLINUX_CONSOLE_PTY_TX_BYTES >=
		       FPLINUX_CONSOLE_TERMINAL_INPUT_FIFO_RESERVE_BYTES,
	       "PTY FIFO must hold one terminal-reply batch and Alt commit");
_Static_assert(FPLINUX_CONSOLE_PTY_TX_BYTES >=
		       FPLINUX_CONSOLE_KEYPAD_INPUT_FIFO_RESERVE_BYTES,
	       "PTY FIFO must hold one keypad batch and Alt commit");

struct terminal_reply {
	char bytes[FPLINUX_CONSOLE_TERMINAL_REPLY_BYTES];
	size_t length;
	struct timespec deadline;
};

struct byte_fifo {
	unsigned char bytes[FPLINUX_CONSOLE_PTY_TX_BYTES];
	size_t head;
	size_t length;
};

#define FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT 3
#define FPLINUX_CONSOLE_REPEAT_DELAY_MS 400U
#define FPLINUX_CONSOLE_REPEAT_PERIOD_MS 40U

struct runtime_state {
	int keypad;
	int extra_keypads[FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT];
	bool keypad_dropping[1 + FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT];
	int pty;
	int signal_fd;
	int tty0;
	int console_lock;
	int primary_vcsa;
	int history_tty;
	int history_vcsa;
	unsigned primary_vt;
	unsigned history_vt;
	int primary_keyboard_mode;
	pid_t child;
	int child_status;
	int shutdown_signal;
	bool child_reaped;
	bool primary_keyboard_saved;
	bool history_active;
	bool pty_final_drain;
	bool cleanup_done;
};

enum modifier {
	FPLINUX_CONSOLE_MODIFIER_NONE,
	FPLINUX_CONSOLE_MODIFIER_CTRL,
	FPLINUX_CONSOLE_MODIFIER_ALT,
	FPLINUX_CONSOLE_MODIFIER_SHIFT,
};

struct composition {
	struct fplinux_multitap multitap;
	enum modifier modifier;
	struct timespec last_press;
	struct timespec deadline;
};

struct vcsa_cell {
	unsigned char character;
	unsigned char attribute;
};

struct vcsa_overlay {
	struct vcsa_cell saved;
	struct vcsa_cell marker;
	off_t offset;
	bool drawn;
};

struct transcript_line {
	unsigned char bytes[FPLINUX_CONSOLE_TRANSCRIPT_LINE_BYTES];
	size_t length;
	size_t cursor;
};

enum transcript_escape_state {
	FPLINUX_CONSOLE_TRANSCRIPT_TEXT,
	FPLINUX_CONSOLE_TRANSCRIPT_ESCAPE,
	FPLINUX_CONSOLE_TRANSCRIPT_ESCAPE_SEQUENCE,
	FPLINUX_CONSOLE_TRANSCRIPT_CSI,
	FPLINUX_CONSOLE_TRANSCRIPT_OSC,
	FPLINUX_CONSOLE_TRANSCRIPT_OSC_ESCAPE,
	FPLINUX_CONSOLE_TRANSCRIPT_STRING,
	FPLINUX_CONSOLE_TRANSCRIPT_STRING_ESCAPE,
};

struct transcript {
	struct transcript_line lines[FPLINUX_CONSOLE_TRANSCRIPT_LINE_COUNT];
	size_t first;
	size_t count;
	enum transcript_escape_state escape_state;
};

struct interface_state {
	unsigned status_row;
	bool raw_input;
	bool raw_toggled;
	bool star_down;
	bool held_star_byte;
	unsigned star_slot;
	uint16_t raw_switch_code;
	unsigned raw_switch_slot;
	struct composition composition;
	struct vcsa_overlay overlay;
	struct transcript transcript;
	struct timespec visual_bell_deadline;
	struct timespec star_hold_deadline;
	size_t history_distance;
	bool visual_bell;
	bool suppress_backspace_until_release;
	unsigned backspace_slot;
	unsigned char history_cells[FPLINUX_CONSOLE_VT_MAX_CELLS * 2];
};

static struct runtime_state runtime = {
	.keypad = -1,
	.extra_keypads = { -1, -1, -1 },
	.pty = -1,
	.signal_fd = -1,
	.tty0 = -1,
	.console_lock = -1,
	.primary_vcsa = -1,
	.history_tty = -1,
	.history_vcsa = -1,
	.child = -1,
};
static struct interface_state interface;
static struct termios saved_console_termios;
static bool console_termios_changed;

static void die(const char *message);
static void die_errno(const char *message);
static void refresh_overlay(void);
static void render_history(void);
static void sync_new_keypad(int slot);

static bool write_all(int fd, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;

	while (length) {
		ssize_t written = write(fd, bytes, length);

		if (written > 0) {
			bytes += written;
			length -= (size_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (!written)
			errno = EIO;
		return false;
	}
	return true;
}

static bool read_all_at(int fd, void *buffer, size_t length, off_t offset)
{
	unsigned char *bytes = buffer;

	while (length) {
		ssize_t received = pread(fd, bytes, length, offset);

		if (received > 0) {
			bytes += received;
			length -= (size_t)received;
			offset += received;
			continue;
		}
		if (received < 0 && errno == EINTR)
			continue;
		if (!received)
			errno = EIO;
		return false;
	}
	return true;
}

static bool write_all_at(int fd, const void *buffer, size_t length,
			 off_t offset)
{
	const unsigned char *bytes = buffer;

	while (length) {
		ssize_t written = pwrite(fd, bytes, length, offset);

		if (written > 0) {
			bytes += written;
			length -= (size_t)written;
			offset += written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (!written)
			errno = EIO;
		return false;
	}
	return true;
}

static int ioctl_value(int fd, unsigned long request, unsigned value)
{
	return ioctl(fd, request, (void *)(uintptr_t)value);
}

/*
 * Raw mode reads K_XLATE bytes from the VT. K_OFF prevents those bytes from
 * duplicating the evdev input composed in multi-tap mode.
 */
static void apply_keyboard_mode(void)
{
	if (!runtime.primary_keyboard_saved)
		return;
	if (ioctl_value(STDIN_FILENO, KDSKBMODE,
			interface.raw_input ? K_XLATE : K_OFF) < 0)
		die_errno("cannot set primary VT keyboard mode");
}

static bool remove_overlay(void)
{
	struct vcsa_cell current;
	struct vcsa_overlay overlay;

	if (!interface.overlay.drawn)
		return true;
	overlay = interface.overlay;
	interface.overlay.drawn = false;
	if (!read_all_at(runtime.primary_vcsa, &current, sizeof(current),
			 overlay.offset))
		return false;
	if (memcmp(&current, &overlay.marker, sizeof(current)) != 0)
		return true;
	return write_all_at(runtime.primary_vcsa, &overlay.saved,
			    sizeof(overlay.saved), overlay.offset);
}

static void write_console(const void *buffer, size_t length)
{
	if (!write_all(STDOUT_FILENO, buffer, length))
		die_errno("cannot write console output");
}

static void putstr(int fd, const char *text)
{
	if (!write_all(fd, text, strlen(text)) && fd == STDOUT_FILENO)
		die_errno("cannot write console output");
}

static void restore_console_terminal(void)
{
	if (!console_termios_changed)
		return;
	tcsetattr(STDIN_FILENO, TCSANOW, &saved_console_termios);
	console_termios_changed = false;
}

static void signal_child_group(int signal_number)
{
	if (runtime.child <= 0 || runtime.child_reaped)
		return;
	if (kill(-runtime.child, signal_number) < 0 && errno == ESRCH)
		kill(runtime.child, signal_number);
}

static bool reap_child_nonblocking(void)
{
	pid_t result;

	if (runtime.child <= 0 || runtime.child_reaped)
		return true;
	do {
		result = waitpid(runtime.child, &runtime.child_status, WNOHANG);
	} while (result < 0 && errno == EINTR);
	if (result == runtime.child || (result < 0 && errno == ECHILD)) {
		runtime.child_reaped = true;
		return true;
	}
	return false;
}

static void cleanup_virtual_terminals(void)
{
	remove_overlay();
	if (runtime.primary_keyboard_saved) {
		ioctl_value(STDIN_FILENO, KDSKBMODE,
			    (unsigned)runtime.primary_keyboard_mode);
		runtime.primary_keyboard_saved = false;
	}
	if (runtime.history_active && runtime.tty0 >= 0 && runtime.primary_vt) {
		ioctl_value(runtime.tty0, VT_ACTIVATE, runtime.primary_vt);
		ioctl_value(runtime.tty0, VT_WAITACTIVE, runtime.primary_vt);
	}
	runtime.history_active = false;
	if (interface.status_row) {
		char sequence[32];
		int length;

		length = snprintf(sequence, sizeof(sequence),
				  "\033[r\033[%u;1H\033[2K",
				  interface.status_row);
		if (length > 0 && (size_t)length < sizeof(sequence))
			write_all(STDOUT_FILENO, sequence, (size_t)length);
		interface.status_row = 0;
	}
	if (runtime.history_vcsa >= 0) {
		close(runtime.history_vcsa);
		runtime.history_vcsa = -1;
	}
	if (runtime.history_tty >= 0) {
		close(runtime.history_tty);
		runtime.history_tty = -1;
	}
	if (runtime.history_vt && runtime.tty0 >= 0)
		ioctl_value(runtime.tty0, VT_DISALLOCATE, runtime.history_vt);
	runtime.history_vt = 0;
	if (runtime.primary_vcsa >= 0) {
		close(runtime.primary_vcsa);
		runtime.primary_vcsa = -1;
	}
	if (runtime.tty0 >= 0) {
		close(runtime.tty0);
		runtime.tty0 = -1;
	}
}

static void cleanup_runtime(void)
{
	struct timespec pause = {
		.tv_sec = 0,
		.tv_nsec = FPLINUX_CONSOLE_CHILD_EXIT_POLL_MS * 1000000L,
	};
	unsigned attempts = FPLINUX_CONSOLE_CHILD_EXIT_GRACE_MS /
			    FPLINUX_CONSOLE_CHILD_EXIT_POLL_MS;
	unsigned i;

	if (runtime.cleanup_done)
		return;
	runtime.cleanup_done = true;

	if (runtime.keypad >= 0) {
		close(runtime.keypad);
		runtime.keypad = -1;
	}
	for (i = 0; i < FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT; ++i) {
		if (runtime.extra_keypads[i] < 0)
			continue;
		close(runtime.extra_keypads[i]);
		runtime.extra_keypads[i] = -1;
	}
	if (runtime.child > 0 && !runtime.child_reaped)
		signal_child_group(runtime.shutdown_signal ?
					   runtime.shutdown_signal :
					   SIGHUP);
	if (runtime.pty >= 0) {
		close(runtime.pty);
		runtime.pty = -1;
	}
	if (runtime.signal_fd >= 0) {
		close(runtime.signal_fd);
		runtime.signal_fd = -1;
	}
	cleanup_virtual_terminals();
	restore_console_terminal();

	for (i = 0; i < attempts && !reap_child_nonblocking(); ++i) {
		struct timespec remaining = pause;

		while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
			;
	}
	if (!reap_child_nonblocking()) {
		signal_child_group(SIGKILL);
		do {
			pid_t result = waitpid(runtime.child,
					       &runtime.child_status, 0);

			if (result == runtime.child ||
			    (result < 0 && errno == ECHILD)) {
				runtime.child_reaped = true;
				break;
			}
			if (result < 0 && errno != EINTR)
				break;
		} while (!runtime.child_reaped);
	}
	if (runtime.console_lock >= 0) {
		close(runtime.console_lock);
		runtime.console_lock = -1;
	}
}

static void report_labelled(const char *label, const char *message,
			    int error_number)
{
	char buffer[256];
	int length;

	if (error_number)
		length = snprintf(buffer, sizeof(buffer),
				  "\r\nFPLINUX-CONSOLE %s: %s: %s\r\n", label,
				  message, strerror(error_number));
	else
		length = snprintf(buffer, sizeof(buffer),
				  "\r\nFPLINUX-CONSOLE %s: %s\r\n", label,
				  message);
	if (length > 0) {
		size_t output_length = (size_t)length;

		if (output_length >= sizeof(buffer))
			output_length = sizeof(buffer) - 1;
		write_all(STDERR_FILENO, buffer, output_length);
	}
}

static void report_error(const char *message, int error_number)
{
	report_labelled("ERROR", message, error_number);
}

static void report_warning(const char *message, int error_number)
{
	report_labelled("WARNING", message, error_number);
}

static void die(const char *message)
{
	report_error(message, 0);
	cleanup_runtime();
	exit(111);
}

static void die_errno(const char *message)
{
	int error_number = errno;

	report_error(message, error_number);
	cleanup_runtime();
	exit(111);
}

static void prepare_console_terminal(void)
{
	struct termios termios;

	if (tcgetattr(STDIN_FILENO, &termios) < 0)
		die_errno("cannot read console termios");
	saved_console_termios = termios;
	cfmakeraw(&termios);
	termios.c_cc[VTIME] = 0;
	termios.c_cc[VMIN] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &termios) < 0)
		die_errno("cannot configure console terminal");
	console_termios_changed = true;
}

static void configure_signals(sigset_t *child_signal_mask)
{
	struct sigaction child_action = {
		.sa_handler = SIG_DFL,
		.sa_flags = SA_NOCLDSTOP,
	};
	sigset_t blocked;

	sigemptyset(&child_action.sa_mask);
	if (sigaction(SIGCHLD, &child_action, NULL) < 0)
		die_errno("cannot restore default SIGCHLD handling");
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGHUP);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGQUIT);
	sigaddset(&blocked, SIGTERM);
	sigaddset(&blocked, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &blocked, child_signal_mask) < 0)
		die_errno("cannot block termination signals");
	runtime.signal_fd = signalfd(-1, &blocked, SFD_NONBLOCK | SFD_CLOEXEC);
	if (runtime.signal_fd < 0)
		die_errno("cannot create signal file descriptor");
}

static bool bit_is_set(const unsigned long *bits, unsigned bit)
{
	return (bits[BIT_WORD(bit)] & BIT_MASK(bit)) != 0;
}

static void set_repeat_rate(int fd)
{
	unsigned int settings[2] = { FPLINUX_CONSOLE_REPEAT_DELAY_MS,
				     FPLINUX_CONSOLE_REPEAT_PERIOD_MS };

	ioctl(fd, EVIOCSREP, settings);
}

static bool is_console_keypad(int fd)
{
	static const unsigned short required_keys[] = {
		KEY_0,	 KEY_1,		KEY_2,	   KEY_3,	   KEY_4,
		KEY_5,	 KEY_6,		KEY_7,	   KEY_8,	   KEY_9,
		KEY_TAB, KEY_BACKSPACE, KEY_ENTER, KEY_KPASTERISK, KEY_KPDOT,
		KEY_UP,	 KEY_LEFT,	KEY_RIGHT, KEY_DOWN,
	};
	unsigned long event_bits[BIT_ARRAY_SIZE(EV_MAX)] = { 0 };
	unsigned long key_bits[BIT_ARRAY_SIZE(KEY_MAX)] = { 0 };
	size_t i;

	if (ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0 ||
	    !bit_is_set(event_bits, EV_KEY) ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
		return false;
	for (i = 0; i < sizeof(required_keys) / sizeof(required_keys[0]); ++i)
		if (!bit_is_set(key_bits, required_keys[i]))
			return false;
	return true;
}

static bool is_event_device_name(const char *name)
{
	const char *digit;
	size_t digits = 0;

	if (strncmp(name, "event", 5) != 0)
		return false;
	digit = name + 5;
	if (!*digit)
		return false;
	while (*digit >= '0' && *digit <= '9') {
		++digit;
		++digits;
	}
	return !*digit && digits <= 10;
}

static void reset_keypad_gesture(int slot)
{
	unsigned token = (unsigned)slot + 1;

	if (interface.backspace_slot == token) {
		interface.suppress_backspace_until_release = false;
		interface.backspace_slot = 0;
	}
	if (interface.star_slot == token) {
		interface.raw_toggled = false;
		interface.star_down = false;
		interface.held_star_byte = false;
		interface.star_slot = 0;
	}
	if (interface.raw_switch_slot == token) {
		interface.raw_switch_code = 0;
		interface.raw_switch_slot = 0;
	}
}

static void drop_keypad_gesture(int slot)
{
	unsigned token = (unsigned)slot + 1;

	if (interface.star_slot == token)
		interface.star_down = false;
	if (interface.raw_switch_slot == token) {
		interface.raw_switch_code = 0;
		interface.raw_switch_slot = 0;
	}
}

static void close_extra_keypad(size_t slot)
{
	if (slot >= FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT ||
	    runtime.extra_keypads[slot] < 0)
		return;
	close(runtime.extra_keypads[slot]);
	runtime.extra_keypads[slot] = -1;
	runtime.keypad_dropping[slot + 1] = false;
	reset_keypad_gesture((int)slot + 1);
}

static void open_extra_keypads(int primary, int *extra)
{
	struct dirent *entry;
	DIR *directory;
	struct stat first;
	size_t entries = 0;
	size_t taken = 0;
	size_t slot;
	int directory_fd;

	if (primary < 0 || fstat(primary, &first) != 0)
		return;
	for (slot = 0; slot < FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT; ++slot) {
		struct stat status;

		if (extra[slot] < 0)
			continue;
		if (fstat(extra[slot], &status) != 0 ||
		    status.st_rdev == first.st_rdev) {
			close_extra_keypad(slot);
			continue;
		}
		++taken;
	}
	directory_fd = open("/dev/input",
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory_fd < 0)
		return;
	directory = fdopendir(directory_fd);
	if (!directory) {
		close(directory_fd);
		return;
	}
	while (taken < FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT &&
	       entries < FPLINUX_CONSOLE_INPUT_DIRECTORY_ENTRY_LIMIT &&
	       (entry = readdir(directory)) != NULL) {
		struct stat status;
		bool duplicate;
		int fd;

		++entries;
		if (!is_event_device_name(entry->d_name))
			continue;
		fd = openat(directory_fd, entry->d_name,
			    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (fstat(fd, &status) != 0 || !S_ISCHR(status.st_mode) ||
		    !is_console_keypad(fd)) {
			close(fd);
			continue;
		}
		duplicate = status.st_rdev == first.st_rdev;
		for (slot = 0;
		     !duplicate && slot < FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT;
		     ++slot) {
			struct stat existing;

			if (extra[slot] >= 0 &&
			    fstat(extra[slot], &existing) == 0 &&
			    existing.st_rdev == status.st_rdev)
				duplicate = true;
		}
		if (duplicate) {
			close(fd);
			continue;
		}
		for (slot = 0; slot < FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT;
		     ++slot)
			if (extra[slot] < 0)
				break;
		if (slot == FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT) {
			close(fd);
			break;
		}
		set_repeat_rate(fd);
		extra[slot] = fd;
		runtime.keypad_dropping[slot + 1] = false;
		sync_new_keypad((int)slot + 1);
		++taken;
	}
	closedir(directory);
}

static int open_keypad(void)
{
	struct dirent *entry;
	DIR *directory;
	size_t entries = 0;
	int directory_fd;

	directory_fd = open("/dev/input",
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory_fd < 0)
		return -1;
	directory = fdopendir(directory_fd);
	if (!directory) {
		close(directory_fd);
		return -1;
	}
	while (entries < FPLINUX_CONSOLE_INPUT_DIRECTORY_ENTRY_LIMIT &&
	       (entry = readdir(directory)) != NULL) {
		struct stat status;
		int fd;

		++entries;

		if (!is_event_device_name(entry->d_name))
			continue;
		fd = openat(directory_fd, entry->d_name,
			    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (fstat(fd, &status) == 0 && S_ISCHR(status.st_mode) &&
		    is_console_keypad(fd)) {
			set_repeat_rate(fd);
			closedir(directory);
			return fd;
		}
		close(fd);
	}
	closedir(directory);
	return -1;
}

static struct winsize console_geometry(void)
{
	struct winsize geometry = { 0 };

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &geometry) < 0)
		die_errno("cannot read console geometry with TIOCGWINSZ");
	if (!geometry.ws_row || !geometry.ws_col)
		die("console geometry has zero rows or columns");
	if (geometry.ws_row > FPLINUX_CONSOLE_VT_MAX_ROWS ||
	    geometry.ws_col > FPLINUX_CONSOLE_VT_MAX_COLS)
		die("console geometry exceeds shared console limits");
	return geometry;
}

static unsigned vt_number_from_name(const char *name)
{
	const char *digits;
	char *end;
	unsigned long number;

	if (strncmp(name, "/dev/tty", 8) != 0)
		return 0;
	digits = name + 8;
	if (!*digits)
		return 0;
	errno = 0;
	number = strtoul(digits, &end, 10);
	if (errno || *end || !number || number > MAX_NR_CONSOLES)
		return 0;
	return (unsigned)number;
}

static bool terminal_fd_matches_device(int fd, const struct stat *expected)
{
	unsigned int encoded;
	unsigned int device_major;
	unsigned int device_minor;

	if (ioctl(fd, TIOCGDEV, &encoded) < 0)
		die_errno("cannot resolve Linux VT device with TIOCGDEV");
	device_major = (encoded & 0x000fff00U) >> 8;
	device_minor = (encoded & 0x000000ffU) |
		       ((encoded >> 12) & 0x000fff00U);
	return device_major == major(expected->st_rdev) &&
	       device_minor == minor(expected->st_rdev);
}

static void validate_vcsa(int fd, unsigned vt, const struct winsize *geometry,
			  const char *message)
{
	unsigned char header[4];
	struct vcsa_cell cell;
	struct stat status;

	if (fstat(fd, &status) < 0)
		die_errno("cannot inspect VCSA device identity");
	if (!S_ISCHR(status.st_mode) ||
	    major(status.st_rdev) != FPLINUX_CONSOLE_VCSA_DEVICE_MAJOR ||
	    minor(status.st_rdev) !=
		    FPLINUX_CONSOLE_VCSA_DEVICE_MINOR_BASE + vt)
		die("VCSA path does not identify the expected Linux device");
	if (!read_all_at(fd, header, sizeof(header), 0))
		die_errno(message);
	if (!header[0] || !header[1] ||
	    header[0] > FPLINUX_CONSOLE_VT_MAX_ROWS ||
	    header[1] > FPLINUX_CONSOLE_VT_MAX_COLS ||
	    header[0] != geometry->ws_row || header[1] != geometry->ws_col)
		die("VCSA geometry does not match the active Linux VT");
	if (!read_all_at(fd, &cell, sizeof(cell), 4) ||
	    !write_all_at(fd, &cell, sizeof(cell), 4))
		die_errno("VCSA device is not writable");
}

/* A failed check must not make the phone lose its only local console. */
static void claim_console(unsigned vt)
{
	struct flock claim = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
	};
	char message[192];
	char path[64];
	int error_number;

	snprintf(path, sizeof(path), "/tmp/fplinux-console.tty%u.lock", vt);
	runtime.console_lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (runtime.console_lock < 0) {
		report_warning(
			"cannot open the local console lock; starting anyway",
			errno);
		return;
	}
	if (fcntl(runtime.console_lock, F_SETLK, &claim) == 0)
		return;
	error_number = errno;
	if (error_number == EACCES || error_number == EAGAIN) {
		struct flock holder = claim;

		if (fcntl(runtime.console_lock, F_GETLK, &holder) == 0 &&
		    holder.l_type != F_UNLCK && holder.l_pid > 0)
			snprintf(
				message, sizeof(message),
				"another FPLinux console already holds /dev/tty%u "
				"(pid %ld); stop it, then start this one again",
				vt, (long)holder.l_pid);
		else
			snprintf(
				message, sizeof(message),
				"another FPLinux console already holds /dev/tty%u; "
				"stop it, then start this one again",
				vt);
		die(message);
	}
	report_warning("cannot lock the local console; starting anyway",
		       error_number);
	close(runtime.console_lock);
	runtime.console_lock = -1;
}

static void setup_virtual_terminals(const struct winsize *geometry)
{
	struct vt_stat state;
	struct stat active_stat;
	struct stat input_stat;
	struct stat output_stat;
	char input_name[64] = { 0 };
	char output_name[64] = { 0 };
	char path[32];
	unsigned input_vt;
	unsigned output_vt;
	int mode;
	int spare = 0;

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
	    fstat(STDIN_FILENO, &input_stat) < 0 ||
	    fstat(STDOUT_FILENO, &output_stat) < 0 ||
	    !S_ISCHR(input_stat.st_mode) || !S_ISCHR(output_stat.st_mode) ||
	    input_stat.st_rdev != output_stat.st_rdev)
		die("stdin and stdout must be the same Linux VT");
	if (ioctl(STDIN_FILENO, KDGETMODE, &mode) < 0 || mode != KD_TEXT ||
	    ioctl(STDOUT_FILENO, KDGETMODE, &mode) < 0 || mode != KD_TEXT)
		die("stdin and stdout must be a Linux VT in text mode");

	runtime.tty0 = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (runtime.tty0 < 0)
		die_errno("cannot open /dev/tty0");
	if (ioctl(runtime.tty0, VT_GETSTATE, &state) < 0)
		die_errno("cannot determine the active Linux VT");
	if (!state.v_active)
		die("VT_GETSTATE reported no active Linux VT");

	if (ttyname_r(STDIN_FILENO, input_name, sizeof(input_name)) != 0)
		input_name[0] = '\0';
	if (ttyname_r(STDOUT_FILENO, output_name, sizeof(output_name)) != 0)
		output_name[0] = '\0';
	input_vt = vt_number_from_name(input_name);
	output_vt = vt_number_from_name(output_name);
	if (input_vt && output_vt && input_vt != output_vt)
		die("stdin and stdout resolve to different Linux VTs");
	runtime.primary_vt = input_vt ? input_vt : output_vt;
	if (!runtime.primary_vt)
		runtime.primary_vt = state.v_active;
	if (runtime.primary_vt != state.v_active)
		die("the console Linux VT is not active at startup");

	snprintf(path, sizeof(path), "/dev/tty%u", runtime.primary_vt);
	if (stat(path, &active_stat) < 0)
		die_errno("cannot inspect the active Linux VT device");
	if (!S_ISCHR(active_stat.st_mode))
		die("the active Linux VT path is not a character device");
	if (!terminal_fd_matches_device(STDIN_FILENO, &active_stat) ||
	    !terminal_fd_matches_device(STDOUT_FILENO, &active_stat))
		die("stdin or stdout does not resolve to the active Linux VT");

	/* Claim before changing the VT so refusal leaves it untouched. */
	claim_console(runtime.primary_vt);

	/*
	 * Terminal query replies are inserted by the VT core, not the keyboard,
	 * so they arrive on stdin whichever mode the keyboard layer is in.
	 */
	if (ioctl(STDIN_FILENO, KDGKBMODE, &runtime.primary_keyboard_mode) < 0)
		die_errno("cannot read primary VT keyboard mode");
	runtime.primary_keyboard_saved = true;
	apply_keyboard_mode();

	snprintf(path, sizeof(path), "/dev/vcsa%u", runtime.primary_vt);
	runtime.primary_vcsa =
		open(path, O_RDWR | O_NOCTTY | O_NOFOLLOW | O_CLOEXEC);
	if (runtime.primary_vcsa < 0)
		die_errno("cannot open primary VCSA device");
	validate_vcsa(runtime.primary_vcsa, runtime.primary_vt, geometry,
		      "cannot read primary VCSA device");

	if (ioctl(runtime.tty0, VT_OPENQRY, &spare) < 0)
		die_errno("cannot query a spare Linux VT for history");
	if (spare <= 0)
		die("no spare Linux VT is available for history");
	runtime.history_vt = (unsigned)spare;
	snprintf(path, sizeof(path), "/dev/tty%u", runtime.history_vt);
	runtime.history_tty = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (runtime.history_tty < 0)
		die_errno("cannot open spare history VT");
	if (ioctl_value(runtime.history_tty, KDSETMODE, KD_TEXT) < 0)
		die_errno("cannot configure spare history VT");
	/*
	 * The VT keyboard handler sees the same input device as evdev.  Disable
	 * translation on the history VT so its line discipline cannot echo
	 * arrow sequences over the cells rendered directly through VCSA.
	 */
	if (ioctl_value(runtime.history_tty, KDSKBMODE, K_OFF) < 0)
		die_errno("cannot disable spare history VT keyboard input");
	snprintf(path, sizeof(path), "/dev/vcsa%u", runtime.history_vt);
	runtime.history_vcsa =
		open(path, O_RDWR | O_NOCTTY | O_NOFOLLOW | O_CLOEXEC);
	if (runtime.history_vcsa < 0)
		die_errno("cannot open history VCSA device");
	validate_vcsa(runtime.history_vcsa, runtime.history_vt, geometry,
		      "cannot read history VCSA device");
}

static int create_shell_pty(const sigset_t *child_signal_mask,
			    const struct winsize *geometry)
{
	char columns[32];
	char lines[32];
	char *slave_path;
	pid_t child;
	int master;

	/*
	 * BusyBox line editing prefers COLUMNS to TIOCGWINSZ and silently
	 * falls back to a width of 80 whenever the value it settles on is
	 * below two, so the geometry is stated in both places.
	 */
	snprintf(columns, sizeof(columns), "COLUMNS=%u",
		 (unsigned)geometry->ws_col);
	snprintf(lines, sizeof(lines), "LINES=%u", (unsigned)geometry->ws_row);

	master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (master < 0)
		die_errno("cannot open /dev/ptmx");
	if (grantpt(master) < 0 || unlockpt(master) < 0)
		die_errno("cannot unlock PTY");
	slave_path = ptsname(master);
	if (!slave_path)
		die_errno("cannot get PTY path");

	child = fork();
	if (child < 0)
		die_errno("fork failed");
	if (child == 0) {
		char *environment[] = {
			"HOME=/root",
			"USER=root",
			"LOGNAME=root",
			"SHELL=/bin/sh",
			"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
			"TERM=linux",
			"PS1=fplinux# ",
			columns,
			lines,
			NULL,
		};
		int slave;

		if (sigprocmask(SIG_SETMASK, child_signal_mask, NULL) < 0)
			_exit(112);
		if (setsid() < 0)
			_exit(112);
		slave = open(slave_path, O_RDWR);
		if (slave < 0 || ioctl(slave, TIOCSCTTY, (void *)0) < 0 ||
		    ioctl(slave, TIOCSWINSZ, geometry) < 0)
			_exit(112);
		if (dup2(slave, STDIN_FILENO) < 0 ||
		    dup2(slave, STDOUT_FILENO) < 0 ||
		    dup2(slave, STDERR_FILENO) < 0)
			_exit(112);
		if (slave > STDERR_FILENO)
			close(slave);
		close(master);
		execle("/bin/sh", "/bin/sh", "-i", NULL, environment);
		_exit(113);
	}

	runtime.child = child;
	return master;
}

static struct timespec monotonic_now(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		die_errno("cannot read monotonic clock");
	return now;
}

static struct timespec deadline_after_ms(struct timespec now, unsigned ms)
{
	now.tv_sec += (time_t)(ms / 1000);
	now.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (now.tv_nsec >= 1000000000L) {
		++now.tv_sec;
		now.tv_nsec -= 1000000000L;
	}
	return now;
}

static bool deadline_reached(struct timespec now, struct timespec deadline)
{
	return now.tv_sec > deadline.tv_sec ||
	       (now.tv_sec == deadline.tv_sec &&
		now.tv_nsec >= deadline.tv_nsec);
}

static uint32_t milliseconds_elapsed(struct timespec now,
				     struct timespec earlier)
{
	time_t seconds;
	long nanoseconds;
	uintmax_t milliseconds;

	if (!deadline_reached(now, earlier))
		return 0;
	seconds = now.tv_sec - earlier.tv_sec;
	nanoseconds = now.tv_nsec - earlier.tv_nsec;
	if (nanoseconds < 0) {
		--seconds;
		nanoseconds += 1000000000L;
	}
	milliseconds =
		(uintmax_t)seconds * 1000U + (uintmax_t)nanoseconds / 1000000U;
	return milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)milliseconds;
}

static int milliseconds_until(struct timespec now, struct timespec deadline)
{
	time_t seconds;
	long nanoseconds;
	long long milliseconds;

	if (deadline_reached(now, deadline))
		return 0;
	seconds = deadline.tv_sec - now.tv_sec;
	nanoseconds = deadline.tv_nsec - now.tv_nsec;
	if (nanoseconds < 0) {
		--seconds;
		nanoseconds += 1000000000L;
	}
	milliseconds =
		(long long)seconds * 1000 + (nanoseconds + 999999L) / 1000000L;
	return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static int earlier_timeout(int current, int candidate)
{
	if (current < 0 || candidate < current)
		return candidate;
	return current;
}

static size_t fifo_free(const struct byte_fifo *fifo)
{
	return sizeof(fifo->bytes) - fifo->length;
}

static bool fifo_push(struct byte_fifo *fifo, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;
	size_t tail;
	size_t first;

	if (length > fifo_free(fifo))
		return false;
	tail = (fifo->head + fifo->length) % sizeof(fifo->bytes);
	first = sizeof(fifo->bytes) - tail;
	if (first > length)
		first = length;
	memcpy(fifo->bytes + tail, bytes, first);
	memcpy(fifo->bytes, bytes + first, length - first);
	fifo->length += length;
	return true;
}

static bool flush_pty_fifo(struct byte_fifo *fifo)
{
	while (fifo->length) {
		size_t contiguous = sizeof(fifo->bytes) - fifo->head;
		ssize_t written;

		if (contiguous > fifo->length)
			contiguous = fifo->length;
		written = write(runtime.pty, fifo->bytes + fifo->head,
				contiguous);
		if (written > 0) {
			fifo->head = (fifo->head + (size_t)written) %
				     sizeof(fifo->bytes);
			fifo->length -= (size_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return true;
		if (written < 0 && (errno == EIO || errno == EPIPE))
			return false;
		if (!written)
			errno = EIO;
		die_errno("cannot write shell PTY");
	}
	return true;
}

static void push_console_bytes(struct byte_fifo *fifo, const void *bytes,
			       size_t length)
{
	if (length && !fifo_push(fifo, bytes, length))
		die("PTY input FIFO capacity invariant violated");
}

/*
 * Linux VT answers terminal status queries through the console input queue,
 * and in raw mode the keyboard layer feeds every key it has translated into
 * the same queue.  A reply belongs to the shell exactly once; everything else
 * is typing and only raw mode passes it on.  A sequence that has started but
 * not yet proved to be a reply is held back until it does, or until
 * release_terminal_hold decides that no reply can still complete it.
 */
static void forward_terminal_replies(struct byte_fifo *fifo,
				     struct terminal_reply *reply,
				     const char *bytes, size_t length)
{
	bool typing = interface.raw_input;
	size_t i;

	for (i = 0; i < length; ++i) {
		unsigned char c = (unsigned char)bytes[i];

		if (!reply->length) {
			if (c != '\033') {
				if (typing && c == '*' && interface.star_down) {
					interface.held_star_byte = true;
					continue;
				}
				if (typing)
					push_console_bytes(fifo, &c, 1);
				continue;
			}
			reply->bytes[reply->length++] = (char)c;
			reply->deadline = deadline_after_ms(
				monotonic_now(),
				FPLINUX_CONSOLE_TERMINAL_REPLY_HOLD_MS);
			continue;
		}
		if (reply->length == 1) {
			if (c == '[') {
				reply->bytes[reply->length++] = (char)c;
				continue;
			}
			if (typing)
				push_console_bytes(fifo, reply->bytes, 1);
			if (c == '\033') {
				reply->deadline = deadline_after_ms(
					monotonic_now(),
					FPLINUX_CONSOLE_TERMINAL_REPLY_HOLD_MS);
				continue;
			}
			reply->length = 0;
			if (typing)
				push_console_bytes(fifo, &c, 1);
			continue;
		}
		if ((c >= '0' && c <= '9') || c == ';' || c == '?' ||
		    c == '>') {
			if (reply->length + 1 < sizeof(reply->bytes)) {
				reply->bytes[reply->length++] = (char)c;
				continue;
			}
		} else if ((c == 'R' || c == 'n' || c == 'c') &&
			   reply->length + 1 <= sizeof(reply->bytes)) {
			reply->bytes[reply->length++] = (char)c;
			push_console_bytes(fifo, reply->bytes, reply->length);
			reply->length = 0;
			continue;
		}
		if (typing) {
			push_console_bytes(fifo, reply->bytes, reply->length);
			push_console_bytes(fifo, &c, 1);
		}
		reply->length = 0;
	}
}

/* A timed-out ESC is keyboard input rather than a terminal reply. */
static void release_terminal_hold(struct byte_fifo *fifo,
				  struct terminal_reply *reply,
				  struct timespec now)
{
	if (!interface.raw_input || !reply->length ||
	    !deadline_reached(now, reply->deadline))
		return;
	push_console_bytes(fifo, reply->bytes, reply->length);
	reply->length = 0;
}

static struct transcript_line *current_transcript_line(void)
{
	size_t index =
		(interface.transcript.first + interface.transcript.count - 1) %
		FPLINUX_CONSOLE_TRANSCRIPT_LINE_COUNT;

	return &interface.transcript.lines[index];
}

static void start_transcript_line(void)
{
	struct transcript *transcript = &interface.transcript;
	size_t index;

	if (transcript->count < FPLINUX_CONSOLE_TRANSCRIPT_LINE_COUNT) {
		index = (transcript->first + transcript->count) %
			FPLINUX_CONSOLE_TRANSCRIPT_LINE_COUNT;
		++transcript->count;
	} else {
		transcript->first = (transcript->first + 1) %
				    FPLINUX_CONSOLE_TRANSCRIPT_LINE_COUNT;
		index = (transcript->first + transcript->count - 1) %
			FPLINUX_CONSOLE_TRANSCRIPT_LINE_COUNT;
	}
	memset(&transcript->lines[index], 0, sizeof(transcript->lines[index]));
	if (runtime.history_active && interface.history_distance)
		++interface.history_distance;
}

static void transcript_put(unsigned char character)
{
	struct transcript_line *line = current_transcript_line();

	if (line->cursor >= sizeof(line->bytes)) {
		start_transcript_line();
		line = current_transcript_line();
	}
	while (line->length < line->cursor)
		line->bytes[line->length++] = ' ';
	line->bytes[line->cursor++] = character;
	if (line->cursor > line->length)
		line->length = line->cursor;
}

static void clear_current_transcript_line(void)
{
	struct transcript_line *line = current_transcript_line();

	memset(line, 0, sizeof(*line));
}

static void ingest_transcript_byte(unsigned char c)
{
	struct transcript *transcript = &interface.transcript;
	struct transcript_line *line;

	if (transcript->escape_state != FPLINUX_CONSOLE_TRANSCRIPT_TEXT &&
	    (c == 0x18 || c == 0x1a || c == 0x9c)) {
		transcript->escape_state = FPLINUX_CONSOLE_TRANSCRIPT_TEXT;
		return;
	}

	switch (transcript->escape_state) {
	case FPLINUX_CONSOLE_TRANSCRIPT_ESCAPE:
		if (c == '[')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_CSI;
		else if (c == ']')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_OSC;
		else if (c == 'P' || c == '^' || c == '_')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_STRING;
		else if (c >= 0x20 && c <= 0x2f)
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_ESCAPE_SEQUENCE;
		else
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_TEXT;
		return;
	case FPLINUX_CONSOLE_TRANSCRIPT_ESCAPE_SEQUENCE:
		if (c >= 0x30 && c <= 0x7e)
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_TEXT;
		return;
	case FPLINUX_CONSOLE_TRANSCRIPT_CSI:
		if (c >= 0x40 && c <= 0x7e) {
			if (c == 'K')
				clear_current_transcript_line();
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_TEXT;
		}
		return;
	case FPLINUX_CONSOLE_TRANSCRIPT_OSC:
		if (c == '\a')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_TEXT;
		else if (c == '\033')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_OSC_ESCAPE;
		return;
	case FPLINUX_CONSOLE_TRANSCRIPT_OSC_ESCAPE:
		if (c == '\\')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_TEXT;
		else if (c != '\033')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_OSC;
		return;
	case FPLINUX_CONSOLE_TRANSCRIPT_STRING:
		if (c == '\033')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_STRING_ESCAPE;
		return;
	case FPLINUX_CONSOLE_TRANSCRIPT_STRING_ESCAPE:
		if (c == '\\')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_TEXT;
		else if (c != '\033')
			transcript->escape_state =
				FPLINUX_CONSOLE_TRANSCRIPT_STRING;
		return;
	case FPLINUX_CONSOLE_TRANSCRIPT_TEXT:
		break;
	}

	if (c == '\033') {
		transcript->escape_state = FPLINUX_CONSOLE_TRANSCRIPT_ESCAPE;
		return;
	}
	if (c == 0x90 || c == 0x9e || c == 0x9f) {
		transcript->escape_state = FPLINUX_CONSOLE_TRANSCRIPT_STRING;
		return;
	}
	if (c == 0x9b) {
		transcript->escape_state = FPLINUX_CONSOLE_TRANSCRIPT_CSI;
		return;
	}
	if (c == 0x9d) {
		transcript->escape_state = FPLINUX_CONSOLE_TRANSCRIPT_OSC;
		return;
	}
	if (c == '\n') {
		start_transcript_line();
		return;
	}
	line = current_transcript_line();
	if (c == '\r') {
		line->cursor = 0;
		return;
	}
	if (c == '\b') {
		if (line->cursor)
			--line->cursor;
		return;
	}
	if (c == '\t') {
		size_t spaces = 8 - line->cursor % 8;

		while (spaces--)
			transcript_put(' ');
		return;
	}
	if (c >= 0x20 && c <= 0x7e)
		transcript_put(c);
	else if (c >= 0xa0)
		transcript_put('?');
}

static void ingest_transcript(const unsigned char *bytes, size_t length)
{
	size_t i;

	for (i = 0; i < length; ++i)
		ingest_transcript_byte(bytes[i]);
}

static unsigned char capital(unsigned char character)
{
	if (character >= 'a' && character <= 'z')
		character -= 'a' - 'A';
	return character;
}

static unsigned char marker_attribute(unsigned char attribute)
{
	unsigned char foreground = attribute & 0x07;
	unsigned char background = (attribute >> 4) & 0x07;

	return (unsigned char)(0x80 | (foreground << 4) | background |
			       (attribute & 0x08));
}

static unsigned char overlay_character(void)
{
	if (interface.visual_bell)
		return '!';
	if (fplinux_multitap_pending(&interface.composition.multitap))
		return fplinux_multitap_candidate(
			&interface.composition.multitap);
	switch (interface.composition.modifier) {
	case FPLINUX_CONSOLE_MODIFIER_CTRL:
		return 'C';
	case FPLINUX_CONSOLE_MODIFIER_ALT:
		return 'A';
	case FPLINUX_CONSOLE_MODIFIER_SHIFT:
		return 'S';
	case FPLINUX_CONSOLE_MODIFIER_NONE:
		return 0;
	}
	return 0;
}

static void draw_overlay(unsigned char character)
{
	unsigned char header[4];
	struct vcsa_cell saved;
	struct vcsa_cell marker;
	off_t offset;

	if (!character || runtime.history_active)
		return;
	if (!read_all_at(runtime.primary_vcsa, header, sizeof(header), 0))
		die_errno("cannot read primary VCSA cursor");
	if (!header[0] || !header[1] ||
	    header[0] > FPLINUX_CONSOLE_VT_MAX_ROWS ||
	    header[1] > FPLINUX_CONSOLE_VT_MAX_COLS || header[2] >= header[1] ||
	    header[3] >= header[0])
		die("primary VCSA reported invalid geometry or cursor");
	offset = 4 + (off_t)2 * ((off_t)header[3] * header[1] + header[2]);
	if (!read_all_at(runtime.primary_vcsa, &saved, sizeof(saved), offset))
		die_errno("cannot save primary VCSA cell");
	marker.character = character;
	marker.attribute = marker_attribute(saved.attribute);
	if (!write_all_at(runtime.primary_vcsa, &marker, sizeof(marker),
			  offset))
		die_errno("cannot draw primary VCSA overlay");
	interface.overlay.saved = saved;
	interface.overlay.marker = marker;
	interface.overlay.offset = offset;
	interface.overlay.drawn = true;
}

/*
 * Written straight into screen memory rather than through the terminal:
 * even a saved and restored cursor motion cancels a pending margin wrap.
 */
static void draw_status_bar(void)
{
	struct vcsa_cell row[FPLINUX_CONSOLE_VT_MAX_COLS];
	unsigned char header[4];
	char text[64];
	const char *mode = interface.raw_input ? "QWERTY" : "T9";
	const char *modifier = "";
	off_t offset;
	size_t columns;
	size_t i;
	int written;

	if (!interface.status_row || runtime.history_active)
		return;
	switch (interface.composition.modifier) {
	case FPLINUX_CONSOLE_MODIFIER_CTRL:
		modifier = " CTRL";
		break;
	case FPLINUX_CONSOLE_MODIFIER_ALT:
		modifier = " ALT";
		break;
	case FPLINUX_CONSOLE_MODIFIER_SHIFT:
		modifier = " SHIFT";
		break;
	default:
		break;
	}
	written = snprintf(text, sizeof(text), " %s%s", mode, modifier);
	if (written < 0)
		die("cannot format status bar text");
	if (!read_all_at(runtime.primary_vcsa, header, sizeof(header), 0))
		die_errno("cannot read primary VCSA geometry");
	if (!header[0] || !header[1] ||
	    header[0] > FPLINUX_CONSOLE_VT_MAX_ROWS ||
	    header[1] > FPLINUX_CONSOLE_VT_MAX_COLS)
		die("primary VCSA reported invalid geometry");
	columns = header[1];
	for (i = 0; i < columns; ++i) {
		row[i].character = ' ';
		row[i].attribute = 0x70;
	}
	for (i = 0; i < columns && i < (size_t)written; ++i)
		row[i].character = (unsigned char)text[i];
	offset = 4 + (off_t)2 * (off_t)(header[0] - 1) * (off_t)columns;
	if (!write_all_at(runtime.primary_vcsa, row, columns * sizeof(row[0]),
			  offset))
		die_errno("cannot draw status bar");
}

static void refresh_overlay(void)
{
	draw_status_bar();
}

static void cancel_composition(void)
{
	fplinux_multitap_cancel(&interface.composition.multitap);
}

static void start_visual_bell(struct timespec now)
{
	interface.visual_bell = true;
	interface.visual_bell_deadline =
		deadline_after_ms(now, FPLINUX_CONSOLE_VISUAL_BELL_MS);
	refresh_overlay();
}

static void dismiss_visual_bell(void)
{
	if (!interface.visual_bell)
		return;
	interface.visual_bell = false;
	refresh_overlay();
}

enum enqueue_result {
	FPLINUX_CONSOLE_ENQUEUE_OK,
	FPLINUX_CONSOLE_ENQUEUE_REJECTED,
	FPLINUX_CONSOLE_ENQUEUE_FULL,
};

static bool compose_character(unsigned char character, unsigned char *bytes,
			      size_t *length)
{
	switch (interface.composition.modifier) {
	case FPLINUX_CONSOLE_MODIFIER_NONE:
		bytes[(*length)++] = character;
		return true;
	case FPLINUX_CONSOLE_MODIFIER_SHIFT:
		bytes[(*length)++] = capital(character);
		return true;
	case FPLINUX_CONSOLE_MODIFIER_ALT:
		bytes[(*length)++] = '\033';
		bytes[(*length)++] = character;
		return true;
	case FPLINUX_CONSOLE_MODIFIER_CTRL:
		if ((character >= '@' && character <= '_') ||
		    (character >= 'a' && character <= 'z')) {
			bytes[(*length)++] = character & 0x1f;
			return true;
		}
		if (character == '?') {
			bytes[(*length)++] = '\177';
			return true;
		}
		return false;
	}
	return false;
}

struct composition_emit_context {
	struct byte_fifo *fifo;
	const char *suffix;
	size_t suffix_length;
};

static enum fplinux_multitap_emit_result
enqueue_composed_character(void *opaque, unsigned char character)
{
	struct composition_emit_context *context = opaque;
	unsigned char bytes[8];
	size_t length = 0;

	if (!compose_character(character, bytes, &length))
		return FPLINUX_MULTITAP_EMIT_REJECTED;
	if (length + context->suffix_length > sizeof(bytes))
		die("key sequence exceeds composition buffer");
	memcpy(bytes + length, context->suffix, context->suffix_length);
	length += context->suffix_length;
	if (!fifo_push(context->fifo, bytes, length))
		return FPLINUX_MULTITAP_EMIT_BLOCKED;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static enum enqueue_result
consume_multitap_result(enum fplinux_multitap_result result,
			struct timespec now)
{
	switch (result) {
	case FPLINUX_MULTITAP_COMMITTED:
		interface.composition.modifier = FPLINUX_CONSOLE_MODIFIER_NONE;
		refresh_overlay();
		return FPLINUX_CONSOLE_ENQUEUE_OK;
	case FPLINUX_MULTITAP_BLOCKED:
		return FPLINUX_CONSOLE_ENQUEUE_FULL;
	case FPLINUX_MULTITAP_REJECTED:
		start_visual_bell(now);
		return FPLINUX_CONSOLE_ENQUEUE_REJECTED;
	case FPLINUX_MULTITAP_PENDING:
	case FPLINUX_MULTITAP_IGNORED:
		return FPLINUX_CONSOLE_ENQUEUE_OK;
	}
	return FPLINUX_CONSOLE_ENQUEUE_FULL;
}

static enum enqueue_result enqueue_composition_and(struct byte_fifo *fifo,
						   const char *suffix,
						   size_t suffix_length,
						   struct timespec now)
{
	struct composition_emit_context context = {
		.fifo = fifo,
		.suffix = suffix,
		.suffix_length = suffix_length,
	};

	if (!fplinux_multitap_pending(&interface.composition.multitap)) {
		if (suffix_length && !fifo_push(fifo, suffix, suffix_length))
			return FPLINUX_CONSOLE_ENQUEUE_FULL;
		return FPLINUX_CONSOLE_ENQUEUE_OK;
	}
	return consume_multitap_result(
		fplinux_multitap_commit(&interface.composition.multitap,
					enqueue_composed_character, &context),
		now);
}

static unsigned char multitap_key(uint16_t code)
{
	if (code == KEY_0)
		return '0';
	if (code >= KEY_1 && code <= KEY_9)
		return (unsigned char)('1' + code - KEY_1);
	return '\0';
}

static bool start_or_cycle_composition(struct byte_fifo *fifo,
				       unsigned char key, struct timespec now)
{
	struct composition_emit_context context = {
		.fifo = fifo,
		.suffix = "",
		.suffix_length = 0,
	};
	enum fplinux_multitap_result result;

	result = fplinux_multitap_press(
		&interface.composition.multitap, key,
		milliseconds_elapsed(now, interface.composition.last_press),
		enqueue_composed_character, &context);
	if (result == FPLINUX_MULTITAP_BLOCKED)
		return false;
	if (result == FPLINUX_MULTITAP_REJECTED) {
		start_visual_bell(now);
		return true;
	}
	if (result == FPLINUX_MULTITAP_IGNORED)
		return true;
	if (result == FPLINUX_MULTITAP_COMMITTED)
		interface.composition.modifier = FPLINUX_CONSOLE_MODIFIER_NONE;
	interface.composition.last_press = now;
	interface.composition.deadline =
		deadline_after_ms(now, FPLINUX_MULTITAP_TIMEOUT_MS);
	refresh_overlay();
	return true;
}

static void cycle_modifier(void)
{
	interface.composition.modifier =
		(enum modifier)((interface.composition.modifier + 1) % 4);
	refresh_overlay();
}

static size_t history_visible_rows(void)
{
	unsigned char header[4];

	if (!read_all_at(runtime.history_vcsa, header, sizeof(header), 0))
		die_errno("cannot read history VCSA geometry");
	if (!header[0] || !header[1] ||
	    header[0] > FPLINUX_CONSOLE_VT_MAX_ROWS ||
	    header[1] > FPLINUX_CONSOLE_VT_MAX_COLS)
		die("history VCSA reported invalid geometry");
	return header[0] > 1 ? (size_t)header[0] - 1 : 0;
}

static size_t history_max_distance(size_t visible_rows)
{
	if (interface.transcript.count <= visible_rows)
		return 0;
	return interface.transcript.count - visible_rows;
}

static void render_history(void)
{
	unsigned char header[4];
	char title[96];
	size_t rows;
	size_t columns;
	size_t cells;
	size_t visible;
	size_t max_distance;
	size_t top;
	size_t row;
	size_t i;
	int title_length;

	if (!runtime.history_active)
		return;
	if (!read_all_at(runtime.history_vcsa, header, sizeof(header), 0))
		die_errno("cannot read history VCSA geometry");
	rows = header[0];
	columns = header[1];
	if (!rows || !columns || rows > FPLINUX_CONSOLE_VT_MAX_ROWS ||
	    columns > FPLINUX_CONSOLE_VT_MAX_COLS)
		die("history VCSA reported invalid geometry");
	cells = rows * columns;
	for (i = 0; i < cells; ++i) {
		interface.history_cells[i * 2] = ' ';
		interface.history_cells[i * 2 + 1] = 0x07;
	}
	visible = rows > 1 ? rows - 1 : 0;
	max_distance = history_max_distance(visible);
	if (interface.history_distance > max_distance)
		interface.history_distance = max_distance;
	top = max_distance - interface.history_distance;
	title_length = snprintf(
		title, sizeof(title),
		"History  Up/Down line  Left/Right page  #/Back exit  OK newest");
	if (title_length < 0)
		die("cannot format history title");
	for (i = 0; i < columns && i < (size_t)title_length; ++i) {
		interface.history_cells[i * 2] = (unsigned char)title[i];
		interface.history_cells[i * 2 + 1] = 0x70;
	}
	for (row = 0; row < visible && top + row < interface.transcript.count;
	     ++row) {
		size_t index = (interface.transcript.first + top + row) %
			       FPLINUX_CONSOLE_TRANSCRIPT_LINE_COUNT;
		const struct transcript_line *line =
			&interface.transcript.lines[index];
		size_t length = line->length < columns ? line->length : columns;
		size_t column;

		for (column = 0; column < length; ++column)
			interface.history_cells[((row + 1) * columns + column) *
						2] = line->bytes[column];
	}
	if (!write_all_at(runtime.history_vcsa, interface.history_cells,
			  cells * 2, 4))
		die_errno("cannot render history VCSA");
}

static void enter_history(void)
{
	if (!remove_overlay())
		die_errno("cannot remove primary VCSA overlay");
	interface.history_distance = 0;
	if (ioctl_value(runtime.tty0, VT_ACTIVATE, runtime.history_vt) < 0)
		die_errno("cannot activate history VT");
	runtime.history_active = true;
	if (ioctl_value(runtime.tty0, VT_WAITACTIVE, runtime.history_vt) < 0)
		die_errno("cannot wait for history VT");
	render_history();
}

static void leave_history(void)
{
	if (ioctl_value(runtime.tty0, VT_ACTIVATE, runtime.primary_vt) < 0 ||
	    ioctl_value(runtime.tty0, VT_WAITACTIVE, runtime.primary_vt) < 0)
		die_errno("cannot return to primary VT");
	runtime.history_active = false;
	refresh_overlay();
}

static void change_history_distance(bool older, size_t amount)
{
	size_t visible = history_visible_rows();
	size_t maximum = history_max_distance(visible);

	if (interface.history_distance > maximum)
		interface.history_distance = maximum;
	if (older) {
		if (amount > maximum - interface.history_distance)
			interface.history_distance = maximum;
		else
			interface.history_distance += amount;
	} else if (amount >= interface.history_distance) {
		interface.history_distance = 0;
	} else {
		interface.history_distance -= amount;
	}
	render_history();
}

static void handle_history_key(uint16_t code, unsigned keypad_token)
{
	size_t page;

	switch (code) {
	case KEY_KPDOT:
		leave_history();
		break;
	case KEY_BACKSPACE:
		interface.suppress_backspace_until_release = true;
		interface.backspace_slot = keypad_token;
		leave_history();
		break;
	case KEY_ENTER:
		interface.history_distance = 0;
		render_history();
		break;
	case KEY_UP:
		change_history_distance(true, 1);
		break;
	case KEY_DOWN:
		change_history_distance(false, 1);
		break;
	case KEY_LEFT:
		page = history_visible_rows();
		change_history_distance(true, page ? page : 1);
		break;
	case KEY_RIGHT:
		page = history_visible_rows();
		change_history_distance(false, page ? page : 1);
		break;
	default:
		break;
	}
}

static bool enqueue_sequence(struct byte_fifo *fifo, const char *sequence)
{
	return fifo_push(fifo, sequence, strlen(sequence));
}

static bool handle_tab(struct byte_fifo *fifo, struct timespec now)
{
	enum enqueue_result result;

	if (fplinux_multitap_pending(&interface.composition.multitap)) {
		result = enqueue_composition_and(fifo, "\t", 1, now);
		return result != FPLINUX_CONSOLE_ENQUEUE_FULL;
	}
	if (interface.composition.modifier == FPLINUX_CONSOLE_MODIFIER_NONE)
		return enqueue_sequence(fifo, "\t");
	if (interface.composition.modifier == FPLINUX_CONSOLE_MODIFIER_SHIFT) {
		if (!enqueue_sequence(fifo, "\033\t"))
			return false;
		interface.composition.modifier = FPLINUX_CONSOLE_MODIFIER_NONE;
		refresh_overlay();
		return true;
	}
	start_visual_bell(now);
	return true;
}

/*
 * A key of the typewriter block that is not one of its modifiers: something
 * the console keymap would type, and which the phone's own keypad does not
 * have at all.
 */
static bool keyboard_key(uint16_t code)
{
	switch (code) {
	case KEY_LEFTCTRL:
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
	case KEY_LEFTALT:
		return false;
	default:
		return code >= KEY_ESC && code <= KEY_SPACE;
	}
}

/*
 * Multi-tap can only express the keys a phone keypad has.  A code it has no
 * meaning for is therefore evidence that something else is being typed on, and
 * the terminal stops interpreting rather than dropping the key.
 */
static bool multitap_handles(uint16_t code)
{
	switch (code) {
	case KEY_BACKSPACE:
	case KEY_KPASTERISK:
	case KEY_KPDOT:
	case KEY_TAB:
	case KEY_ENTER:
	case KEY_UP:
	case KEY_DOWN:
	case KEY_LEFT:
	case KEY_RIGHT:
		return true;
	default:
		return fplinux_multitap_handles(multitap_key(code));
	}
}

static bool handle_primary_key(struct byte_fifo *fifo, uint16_t code,
			       struct timespec now)
{
	unsigned char key = multitap_key(code);
	enum enqueue_result result;
	const char *sequence = NULL;

	dismiss_visual_bell();
	if (key)
		return start_or_cycle_composition(fifo, key, now);

	if (code == KEY_BACKSPACE) {
		if (fplinux_multitap_pending(&interface.composition.multitap)) {
			cancel_composition();
			refresh_overlay();
			return true;
		}
		return enqueue_sequence(fifo, "\177");
	}
	if (code == KEY_KPASTERISK) {
		result = enqueue_composition_and(fifo, "", 0, now);
		if (result == FPLINUX_CONSOLE_ENQUEUE_FULL)
			return false;
		if (result == FPLINUX_CONSOLE_ENQUEUE_OK)
			cycle_modifier();
		return true;
	}
	if (code == KEY_KPDOT) {
		result = enqueue_composition_and(fifo, "", 0, now);
		if (result == FPLINUX_CONSOLE_ENQUEUE_FULL)
			return false;
		if (result == FPLINUX_CONSOLE_ENQUEUE_OK)
			enter_history();
		return true;
	}
	if (code == KEY_TAB)
		return handle_tab(fifo, now);

	switch (code) {
	case KEY_ENTER:
		sequence = "\r";
		break;
	case KEY_UP:
		sequence = "\033[A";
		break;
	case KEY_DOWN:
		sequence = "\033[B";
		break;
	case KEY_RIGHT:
		sequence = "\033[C";
		break;
	case KEY_LEFT:
		sequence = "\033[D";
		break;
	default:
		return true;
	}
	result = enqueue_composition_and(fifo, sequence, strlen(sequence), now);
	return result != FPLINUX_CONSOLE_ENQUEUE_FULL;
}

static bool is_repeatable_key(uint16_t code)
{
	return code == KEY_BACKSPACE || code == KEY_UP || code == KEY_DOWN ||
	       code == KEY_LEFT || code == KEY_RIGHT;
}

static void disconnect_keypad(struct timespec *reopen_deadline,
			      struct timespec now)
{
	if (runtime.keypad >= 0)
		close(runtime.keypad);
	runtime.keypad = -1;
	runtime.keypad_dropping[0] = false;
	cancel_composition();
	interface.composition.modifier = FPLINUX_CONSOLE_MODIFIER_NONE;
	interface.visual_bell = false;
	reset_keypad_gesture(0);
	refresh_overlay();
	*reopen_deadline =
		deadline_after_ms(now, FPLINUX_CONSOLE_KEYPAD_REOPEN_MS);
}

enum pty_drain_result {
	FPLINUX_CONSOLE_PTY_DRAIN_BUDGET_EXHAUSTED,
	FPLINUX_CONSOLE_PTY_DRAIN_IDLE,
	FPLINUX_CONSOLE_PTY_DRAIN_CLOSED,
};

static enum pty_drain_result drain_shell_output(void)
{
	unsigned char output[FPLINUX_CONSOLE_PTY_DRAIN_CHUNK_BYTES];
	enum pty_drain_result result =
		FPLINUX_CONSOLE_PTY_DRAIN_BUDGET_EXHAUSTED;
	size_t bytes = 0;
	size_t reads = 0;
	bool forwarded = false;

	while (bytes < FPLINUX_CONSOLE_PTY_DRAIN_BUDGET_BYTES &&
	       reads < FPLINUX_CONSOLE_PTY_DRAIN_READ_BUDGET) {
		size_t capacity = sizeof(output);
		ssize_t length;

		if (capacity > FPLINUX_CONSOLE_PTY_DRAIN_BUDGET_BYTES - bytes)
			capacity =
				FPLINUX_CONSOLE_PTY_DRAIN_BUDGET_BYTES - bytes;
		++reads;
		length = read(runtime.pty, output, capacity);
		if (length > 0) {
			if (!forwarded && !remove_overlay())
				die_errno("cannot remove primary VCSA overlay");
			forwarded = true;
			write_console(output, (size_t)length);
			ingest_transcript(output, (size_t)length);
			bytes += (size_t)length;
			continue;
		}
		if (!length) {
			result = FPLINUX_CONSOLE_PTY_DRAIN_CLOSED;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			result = FPLINUX_CONSOLE_PTY_DRAIN_IDLE;
			break;
		}
		if (errno == EIO) {
			result = FPLINUX_CONSOLE_PTY_DRAIN_CLOSED;
			break;
		}
		die_errno("cannot read shell PTY");
	}
	if (forwarded) {
		if (runtime.history_active)
			render_history();
		else
			refresh_overlay();
	}
	return result;
}

static void close_shell_pty(struct byte_fifo *fifo)
{
	if (runtime.pty >= 0)
		close(runtime.pty);
	runtime.pty = -1;
	runtime.pty_final_drain = false;
	fifo->head = 0;
	fifo->length = 0;
	reap_child_nonblocking();
}

static int process_signal_events(void)
{
	for (;;) {
		struct signalfd_siginfo signal_info;
		ssize_t length = read(runtime.signal_fd, &signal_info,
				      sizeof(signal_info));

		if (length == (ssize_t)sizeof(signal_info)) {
			switch (signal_info.ssi_signo) {
			case SIGCHLD:
				reap_child_nonblocking();
				break;
			case SIGHUP:
			case SIGINT:
			case SIGQUIT:
			case SIGTERM:
				runtime.shutdown_signal =
					(int)signal_info.ssi_signo;
				return runtime.shutdown_signal;
			default:
				break;
			}
			continue;
		}
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (!length)
			die("signal file descriptor closed");
		if (length > 0)
			die("short read from signal file descriptor");
		die_errno("cannot read signal file descriptor");
	}
}

static int child_exit_code(void)
{
	if (WIFEXITED(runtime.child_status))
		return WEXITSTATUS(runtime.child_status);
	if (WIFSIGNALED(runtime.child_status))
		return 128 + WTERMSIG(runtime.child_status);
	return 111;
}

static void commit_expired_composition(struct byte_fifo *pty_tx,
				       struct timespec now)
{
	struct composition_emit_context context = {
		.fifo = pty_tx,
		.suffix = "",
		.suffix_length = 0,
	};
	enum enqueue_result result;

	if (!fplinux_multitap_pending(&interface.composition.multitap))
		return;
	result = consume_multitap_result(
		fplinux_multitap_expire(
			&interface.composition.multitap,
			milliseconds_elapsed(now,
					     interface.composition.last_press),
			enqueue_composed_character, &context),
		now);
	if (result == FPLINUX_CONSOLE_ENQUEUE_FULL)
		die("PTY input FIFO capacity invariant violated");
}

static void process_keypad_event(struct byte_fifo *pty_tx, int slot,
				 uint16_t code, int32_t value)
{
	struct timespec now;
	unsigned token = (unsigned)slot + 1;

	if (code == KEY_BACKSPACE && value == 0) {
		if (interface.backspace_slot == token) {
			interface.suppress_backspace_until_release = false;
			interface.backspace_slot = 0;
			return;
		}
	}
	if (code == KEY_BACKSPACE &&
	    interface.suppress_backspace_until_release &&
	    interface.backspace_slot == token)
		return;
	/*
	 * This key means one thing tapped and another held, so its tap can
	 * only be known once it is let go without having been held.  The
	 * hold is timed here because the keypad driver need not generate
	 * the auto-repeat events the kernel would time.
	 */
	if (code == KEY_KPASTERISK) {
		bool tapped = value == 0 && !interface.raw_toggled;

		if (value == 1) {
			if (interface.star_slot && interface.star_slot != token)
				return;
			now = monotonic_now();
			interface.star_slot = token;
			interface.star_down = true;
			interface.held_star_byte = false;
			interface.star_hold_deadline = deadline_after_ms(
				now, FPLINUX_CONSOLE_STAR_HOLD_MS);
			return;
		}
		if (value == 0) {
			bool held_star_byte = interface.held_star_byte;

			if (interface.star_slot != token)
				return;
			interface.star_down = false;
			interface.raw_toggled = false;
			interface.held_star_byte = false;
			interface.star_slot = 0;
			if (tapped && interface.raw_input &&
			    !runtime.history_active && held_star_byte) {
				unsigned char star = '*';

				push_console_bytes(pty_tx, &star, 1);
				return;
			}
		}
		if (!tapped)
			return;
		value = 1;
	}
	/*
	 * Raw mode is the kernel's to translate: evdev is still watched, but
	 * only so the star hold above can leave it again.  History is the one
	 * exception, because there the foreground VT is the history one, whose
	 * keyboard is off and which nothing else would drive.
	 */
	if (interface.raw_input && !runtime.history_active)
		return;
	/*
	 * The driver announces KEY_STATUS presses and releases of its own
	 * accord: only a press and release of a code that a keyboard has
	 * proves a keyboard and may end multi-tap here.
	 */
	if (!interface.raw_input && !runtime.history_active &&
	    !multitap_handles(code)) {
		if (!keyboard_key(code))
			return;
		if (value == 1) {
			interface.raw_switch_code = code;
			interface.raw_switch_slot = token;
		}
		if (value == 0 && code == interface.raw_switch_code &&
		    interface.raw_switch_slot == token) {
			interface.raw_switch_code = 0;
			interface.raw_switch_slot = 0;
			interface.raw_input = true;
			apply_keyboard_mode();
			cancel_composition();
			refresh_overlay();
		}
		return;
	}
	if (value != 1 && !(value == 2 && is_repeatable_key(code)))
		return;
	now = monotonic_now();
	commit_expired_composition(pty_tx, now);
	if (runtime.history_active)
		handle_history_key(code, token);
	else if (!handle_primary_key(pty_tx, code, now))
		die("PTY input FIFO capacity invariant violated");
}

static int keypad_slot_fd(int slot)
{
	if (slot == 0)
		return runtime.keypad;
	if (slot > 0 && slot <= FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT)
		return runtime.extra_keypads[slot - 1];
	return -1;
}

static void resync_keypad(int slot)
{
	unsigned char keys[FPLINUX_CONSOLE_KEY_STATE_BYTES] = { 0 };
	int fd = keypad_slot_fd(slot);
	unsigned token = (unsigned)slot + 1;
	bool star_pressed;
	bool backspace_pressed;

	if (fd < 0 || ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) {
		reset_keypad_gesture(slot);
		return;
	}
	star_pressed = keys[KEY_KPASTERISK / 8U] &
		       (unsigned char)(1U << (KEY_KPASTERISK % 8U));
	backspace_pressed = keys[KEY_BACKSPACE / 8U] &
			    (unsigned char)(1U << (KEY_BACKSPACE % 8U));
	if (interface.star_slot == token) {
		if (star_pressed)
			interface.star_down = true;
		else
			reset_keypad_gesture(slot);
	} else if (!interface.star_slot && star_pressed) {
		struct timespec now = monotonic_now();

		interface.star_slot = token;
		interface.star_down = true;
		interface.star_hold_deadline =
			deadline_after_ms(now, FPLINUX_CONSOLE_STAR_HOLD_MS);
	}
	if (interface.backspace_slot == token && !backspace_pressed) {
		interface.suppress_backspace_until_release = false;
		interface.backspace_slot = 0;
	} else if (!interface.backspace_slot && backspace_pressed) {
		interface.suppress_backspace_until_release = true;
		interface.backspace_slot = token;
	}
}

static void sync_new_keypad(int slot)
{
	unsigned token = (unsigned)slot + 1;

	resync_keypad(slot);
	if (interface.star_slot == token && interface.star_down)
		interface.raw_toggled = true;
}

static void process_keypad_events(struct byte_fifo *pty_tx, int slot,
				  const struct input_event *events,
				  size_t count)
{
	size_t i;

	for (i = 0; i < count; ++i) {
		if (events[i].type == EV_SYN && events[i].code == SYN_DROPPED) {
			runtime.keypad_dropping[slot] = true;
			drop_keypad_gesture(slot);
			continue;
		}
		if (runtime.keypad_dropping[slot]) {
			if (events[i].type == EV_SYN &&
			    events[i].code == SYN_REPORT) {
				runtime.keypad_dropping[slot] = false;
				resync_keypad(slot);
			}
			continue;
		}
		if (events[i].type != EV_KEY)
			continue;
		process_keypad_event(pty_tx, slot, events[i].code,
				     events[i].value);
	}
}

int main(void)
{
	struct terminal_reply terminal_reply = { { 0 }, 0, { 0, 0 } };
	struct byte_fifo pty_tx = { { 0 }, 0, 0 };
	struct input_event events[FPLINUX_CONSOLE_KEYPAD_EVENT_COUNT];
	struct pollfd descriptors[4 + FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT];
	struct timespec keypad_reopen_deadline = { 0 };
	struct timespec extra_rescan_deadline = { 0 };
	struct winsize geometry;
	sigset_t child_signal_mask;

	if (atexit(cleanup_runtime) != 0)
		die("cannot register terminal cleanup");
	configure_signals(&child_signal_mask);
	runtime.keypad = open_keypad();
	open_extra_keypads(runtime.keypad, runtime.extra_keypads);
	sync_new_keypad(0);
	geometry = console_geometry();
	setup_virtual_terminals(&geometry);
	/* Keep the bottom row outside the shell's scrolling region. */
	interface.status_row = geometry.ws_row;
	if (interface.status_row && geometry.ws_row > 1)
		geometry.ws_row -= 1;
	prepare_console_terminal();
	interface.transcript.count = 1;

	putstr(STDOUT_FILENO, "\033[2J\033[H");
	if (interface.status_row > 1) {
		char region[32];

		snprintf(region, sizeof(region), "\033[1;%ur\033[H",
			 interface.status_row - 1);
		putstr(STDOUT_FILENO, region);
	}
	draw_status_bar();
	putstr(STDOUT_FILENO, "FPLinux local console\r\n");
	if (runtime.keypad >= 0)
		putstr(STDOUT_FILENO,
		       "Physical keypad event device is connected.\r\n");
	else
		putstr(STDOUT_FILENO, "Waiting for a compatible physical "
				      "keypad event device.\r\n");
	putstr(STDOUT_FILENO,
	       "0-9 T9 multi-tap, tap * modifier, "
	       "hold * T9/QWERTY, # history, soft-right backspace.\r\n\r\n");
	/* Start the shell after the banner so its first prompt has a stable
	 * cell. */
	runtime.pty = create_shell_pty(&child_signal_mask, &geometry);

	for (;;) {
		struct timespec now = monotonic_now();
		int timeout = -1;
		int poll_errno;
		int ready;

		commit_expired_composition(&pty_tx, now);
		release_terminal_hold(&pty_tx, &terminal_reply, now);
		if (interface.visual_bell &&
		    deadline_reached(now, interface.visual_bell_deadline)) {
			interface.visual_bell = false;
			refresh_overlay();
		}
		if (interface.star_down && !interface.raw_toggled &&
		    deadline_reached(now, interface.star_hold_deadline)) {
			interface.raw_toggled = true;
			interface.held_star_byte = false;
			interface.raw_input = !interface.raw_input;
			apply_keyboard_mode();
			cancel_composition();
			refresh_overlay();
		}
		if (runtime.keypad < 0 &&
		    deadline_reached(now, keypad_reopen_deadline)) {
			runtime.keypad = open_keypad();
			if (runtime.keypad < 0) {
				keypad_reopen_deadline = deadline_after_ms(
					now, FPLINUX_CONSOLE_KEYPAD_REOPEN_MS);
			} else {
				runtime.keypad_dropping[0] = false;
				open_extra_keypads(runtime.keypad,
						   runtime.extra_keypads);
				sync_new_keypad(0);
				extra_rescan_deadline = deadline_after_ms(
					now,
					FPLINUX_CONSOLE_KEYPAD_REOPEN_MS * 4);
			}
		}
		if (deadline_reached(now, extra_rescan_deadline)) {
			open_extra_keypads(runtime.keypad,
					   runtime.extra_keypads);
			extra_rescan_deadline = deadline_after_ms(
				now, FPLINUX_CONSOLE_KEYPAD_REOPEN_MS * 4);
		}

		descriptors[0].fd = runtime.keypad;
		descriptors[0].events =
			runtime.pty >= 0 && !runtime.pty_final_drain &&
					fifo_free(&pty_tx) >=
						FPLINUX_CONSOLE_KEYPAD_INPUT_FIFO_RESERVE_BYTES ?
				POLLIN :
				0;
		descriptors[0].revents = 0;
		descriptors[1].fd = runtime.pty;
		descriptors[1].events = runtime.pty >= 0 ? POLLIN : 0;
		if (runtime.pty >= 0 && pty_tx.length)
			descriptors[1].events |= POLLOUT;
		descriptors[1].revents = 0;
		descriptors[2].fd = runtime.pty >= 0 &&
						    !runtime.pty_final_drain ?
					    STDIN_FILENO :
					    -1;
		descriptors[2].events =
			runtime.pty >= 0 && !runtime.pty_final_drain &&
					fifo_free(&pty_tx) >=
						FPLINUX_CONSOLE_TERMINAL_INPUT_FIFO_RESERVE_BYTES ?
				POLLIN :
				0;
		descriptors[2].revents = 0;
		descriptors[3].fd = runtime.signal_fd;
		descriptors[3].events = POLLIN;
		descriptors[3].revents = 0;

		if (fplinux_multitap_pending(&interface.composition.multitap))
			timeout = earlier_timeout(
				timeout,
				milliseconds_until(
					now, interface.composition.deadline));
		if (interface.visual_bell)
			timeout = earlier_timeout(
				timeout,
				milliseconds_until(
					now, interface.visual_bell_deadline));
		if (interface.star_down && !interface.raw_toggled)
			timeout = earlier_timeout(
				timeout,
				milliseconds_until(
					now, interface.star_hold_deadline));
		if (interface.raw_input && terminal_reply.length)
			timeout = earlier_timeout(
				timeout, milliseconds_until(
						 now, terminal_reply.deadline));
		if (runtime.keypad < 0)
			timeout = earlier_timeout(
				timeout, milliseconds_until(
						 now, keypad_reopen_deadline));
		if (runtime.pty_final_drain)
			timeout = 0;

		for (size_t slot = 0; slot < FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT;
		     ++slot) {
			descriptors[4 + slot].fd = runtime.extra_keypads[slot];
			descriptors[4 + slot].events =
				runtime.pty >= 0 && !runtime.pty_final_drain &&
						fifo_free(&pty_tx) >=
							FPLINUX_CONSOLE_KEYPAD_INPUT_FIFO_RESERVE_BYTES ?
					POLLIN :
					0;
			descriptors[4 + slot].revents = 0;
		}
		timeout = earlier_timeout(
			timeout,
			milliseconds_until(now, extra_rescan_deadline));

		/* The candidate exists only while this process is blocked in
		 * poll. */
		draw_overlay(overlay_character());
		ready = poll(descriptors,
			     4 + FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT, timeout);
		poll_errno = errno;
		if (!remove_overlay())
			die_errno("cannot remove primary VCSA overlay");
		errno = poll_errno;
		if (ready < 0 && poll_errno == EINTR)
			continue;
		if (ready < 0)
			die_errno("console poll failed");
		if (!ready && !runtime.pty_final_drain)
			continue;

		if (descriptors[3].revents & POLLIN) {
			int signal_number = process_signal_events();

			if (signal_number)
				return 128 + signal_number;
		}
		if (descriptors[3].revents & (POLLHUP | POLLERR | POLLNVAL))
			die("signal file descriptor poll failure");

		if (runtime.pty >= 0 && runtime.child_reaped)
			runtime.pty_final_drain = true;
		if (runtime.pty >= 0 &&
		    descriptors[1].revents & (POLLHUP | POLLERR))
			runtime.pty_final_drain = true;
		if (runtime.pty >= 0 && (runtime.pty_final_drain ||
					 descriptors[1].revents & POLLIN)) {
			enum pty_drain_result result = drain_shell_output();

			if (result == FPLINUX_CONSOLE_PTY_DRAIN_CLOSED ||
			    (runtime.pty_final_drain &&
			     result == FPLINUX_CONSOLE_PTY_DRAIN_IDLE))
				close_shell_pty(&pty_tx);
		}
		if (runtime.pty >= 0 && descriptors[1].revents & POLLNVAL)
			die("shell PTY poll failure");
		if (runtime.pty >= 0 && !runtime.pty_final_drain &&
		    descriptors[1].revents & POLLOUT)
			if (!flush_pty_fifo(&pty_tx))
				close_shell_pty(&pty_tx);

		if (runtime.keypad >= 0 &&
		    descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
			now = monotonic_now();
			disconnect_keypad(&keypad_reopen_deadline, now);
		} else if (runtime.keypad >= 0 && !runtime.pty_final_drain &&
			   descriptors[0].revents & POLLIN &&
			   fifo_free(&pty_tx) >=
				   FPLINUX_CONSOLE_KEYPAD_INPUT_FIFO_RESERVE_BYTES) {
			ssize_t length;

			do {
				length = read(runtime.keypad, events,
					      sizeof(events));
			} while (length < 0 && errno == EINTR);
			if (!length) {
				now = monotonic_now();
				disconnect_keypad(&keypad_reopen_deadline, now);
			} else if (length < 0 && errno != EAGAIN &&
				   errno != EWOULDBLOCK) {
				now = monotonic_now();
				disconnect_keypad(&keypad_reopen_deadline, now);
			} else if (length > 0) {
				process_keypad_events(
					&pty_tx, 0, events,
					(size_t)length / sizeof(events[0]));
			}
		}

		for (size_t slot = 0; slot < FPLINUX_CONSOLE_EXTRA_KEYPAD_COUNT;
		     ++slot) {
			ssize_t length;

			if (runtime.extra_keypads[slot] < 0)
				continue;
			if (descriptors[4 + slot].revents &
			    (POLLHUP | POLLERR | POLLNVAL)) {
				close_extra_keypad(slot);
				continue;
			}
			if (!(descriptors[4 + slot].revents & POLLIN) ||
			    runtime.pty_final_drain ||
			    fifo_free(&pty_tx) <
				    FPLINUX_CONSOLE_KEYPAD_INPUT_FIFO_RESERVE_BYTES)
				continue;
			do {
				length = read(runtime.extra_keypads[slot],
					      events, sizeof(events));
			} while (length < 0 && errno == EINTR);
			if (length > 0) {
				process_keypad_events(
					&pty_tx, (int)slot + 1, events,
					(size_t)length / sizeof(events[0]));
			} else if (!length ||
				   (errno != EAGAIN && errno != EWOULDBLOCK)) {
				close_extra_keypad(slot);
			}
		}

		if (runtime.pty >= 0 && !runtime.pty_final_drain &&
		    descriptors[2].revents & POLLIN &&
		    fifo_free(&pty_tx) >=
			    FPLINUX_CONSOLE_TERMINAL_INPUT_FIFO_RESERVE_BYTES) {
			char terminal_input[FPLINUX_CONSOLE_TERMINAL_INPUT_BYTES];
			ssize_t length;

			do {
				length = read(STDIN_FILENO, terminal_input,
					      sizeof(terminal_input));
			} while (length < 0 && errno == EINTR);
			if (length > 0) {
				forward_terminal_replies(&pty_tx,
							 &terminal_reply,
							 terminal_input,
							 (size_t)length);
			} else if (!length)
				die("console input closed");
			else if (errno != EAGAIN && errno != EWOULDBLOCK)
				die_errno("cannot read console input");
		}
		if (runtime.pty >= 0 && !runtime.pty_final_drain &&
		    descriptors[2].revents & (POLLHUP | POLLERR | POLLNVAL))
			die("console input poll failure");

		if (runtime.child_reaped && runtime.pty < 0)
			return child_exit_code();
	}
}
