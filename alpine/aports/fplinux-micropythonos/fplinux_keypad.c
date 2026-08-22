// SPDX-License-Identifier: MIT
/* FPLinux normalized evdev keypad module for MicroPython. */
/* fplinux-check: package-embedded */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "py/obj.h"
#include "py/runtime.h"

#define FPLINUX_KEYPAD_PHYS "fplinux/keypad0"
#define FPLINUX_KEYPAD_INPUT_DIRECTORY "/dev/input"
#define FPLINUX_KEYPAD_PATH_BYTES 64U
#define FPLINUX_KEYPAD_PHYS_BYTES 128U
#define FPLINUX_KEYPAD_WORD_BITS (sizeof(unsigned long) * CHAR_BIT)
#define FPLINUX_KEYPAD_BIT_WORDS ((KEY_MAX / FPLINUX_KEYPAD_WORD_BITS) + 1U)

static int keypad_fd = -1;
static bool keypad_grabbed;
static bool sync_dropped;
static uint16_t last_code;
static char opened_path[FPLINUX_KEYPAD_PATH_BYTES];

static bool is_event_name(const char *name)
{
	const char *character;

	if (strncmp(name, "event", 5) != 0 || name[5] == '\0')
		return false;
	for (character = name + 5; *character != '\0'; character++) {
		if (*character < '0' || *character > '9')
			return false;
	}
	return true;
}

static bool bit_is_set(const unsigned long *bits, unsigned int bit)
{
	return (bits[bit / FPLINUX_KEYPAD_WORD_BITS] &
		(1UL << (bit % FPLINUX_KEYPAD_WORD_BITS))) != 0;
}

static bool is_normalized_key(unsigned int code)
{
	switch (code) {
	case KEY_0:
	case KEY_1:
	case KEY_2:
	case KEY_3:
	case KEY_4:
	case KEY_5:
	case KEY_6:
	case KEY_7:
	case KEY_8:
	case KEY_9:
	case KEY_KPASTERISK:
	case KEY_KPDOT:
	case KEY_TAB:
	case KEY_BACKSPACE:
	case KEY_ENTER:
	case KEY_UP:
	case KEY_DOWN:
	case KEY_LEFT:
	case KEY_RIGHT:
		return true;
	default:
		return false;
	}
}

static bool has_normalized_keys(int fd)
{
	unsigned long keys[FPLINUX_KEYPAD_BIT_WORDS] = {};
	static const unsigned int required[] = {
		KEY_0,		KEY_1,	   KEY_2,    KEY_3,	    KEY_4,
		KEY_5,		KEY_6,	   KEY_7,    KEY_8,	    KEY_9,
		KEY_KPASTERISK, KEY_KPDOT, KEY_TAB,  KEY_BACKSPACE, KEY_ENTER,
		KEY_UP,		KEY_DOWN,  KEY_LEFT, KEY_RIGHT,
	};
	size_t index;

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
		return false;
	for (index = 0; index < sizeof(required) / sizeof(required[0]);
	     index++) {
		if (!bit_is_set(keys, required[index]))
			return false;
	}
	return true;
}

static int discover_keypad(char *path, size_t path_length)
{
	DIR *directory;
	struct dirent *entry;
	int found = -1;
	int saved_errno = ENODEV;

	directory = opendir(FPLINUX_KEYPAD_INPUT_DIRECTORY);
	if (directory == NULL)
		return -1;
	while ((entry = readdir(directory)) != NULL) {
		char candidate[FPLINUX_KEYPAD_PATH_BYTES];
		char phys[FPLINUX_KEYPAD_PHYS_BYTES] = {};
		struct stat status;
		int fd;
		int length;

		if (!is_event_name(entry->d_name))
			continue;
		length = snprintf(candidate, sizeof(candidate), "%s/%s",
				  FPLINUX_KEYPAD_INPUT_DIRECTORY,
				  entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(candidate))
			continue;
		fd = open(candidate,
			  O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			saved_errno = errno;
			continue;
		}
		if (fstat(fd, &status) == 0 && S_ISCHR(status.st_mode) &&
		    ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys) >= 0 &&
		    strcmp(phys, FPLINUX_KEYPAD_PHYS) == 0 &&
		    has_normalized_keys(fd)) {
			if (strlen(candidate) + 1U <= path_length) {
				strcpy(path, candidate);
				found = fd;
				break;
			}
			saved_errno = ENAMETOOLONG;
		}
		close(fd);
	}
	closedir(directory);
	if (found < 0)
		errno = saved_errno;
	return found;
}

static void close_keypad(void)
{
	if (keypad_fd >= 0) {
		if (keypad_grabbed)
			(void)ioctl(keypad_fd, EVIOCGRAB, 0);
		close(keypad_fd);
	}
	keypad_fd = -1;
	keypad_grabbed = false;
	sync_dropped = false;
	last_code = 0;
	opened_path[0] = '\0';
}

static mp_obj_t keypad_open(size_t n_args, const mp_obj_t *args)
{
	bool grab = n_args == 0 || mp_obj_is_true(args[0]);

	close_keypad();
	keypad_fd = discover_keypad(opened_path, sizeof(opened_path));
	if (keypad_fd < 0)
		mp_raise_OSError(errno);
	if (grab && ioctl(keypad_fd, EVIOCGRAB, 1) < 0) {
		int error = errno;

		close_keypad();
		mp_raise_OSError(error);
	}
	keypad_grabbed = grab;
	return mp_obj_new_str(opened_path, strlen(opened_path));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(keypad_open_obj, 0, 1, keypad_open);

static mp_obj_t keypad_close(void)
{
	close_keypad();
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(keypad_close_obj, keypad_close);

static mp_obj_t keypad_device(void)
{
	if (keypad_fd < 0)
		return mp_const_none;
	return mp_obj_new_str(opened_path, strlen(opened_path));
}
static MP_DEFINE_CONST_FUN_OBJ_0(keypad_device_obj, keypad_device);

static mp_obj_t event_tuple(uint16_t code, int32_t value)
{
	mp_obj_t values[2] = {
		mp_obj_new_int_from_uint(code),
		mp_obj_new_int(value),
	};

	return mp_obj_new_tuple(2, values);
}

static mp_obj_t resync_state(void)
{
	unsigned long keys[FPLINUX_KEYPAD_BIT_WORDS] = {};
	unsigned int code;

	if (ioctl(keypad_fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
		mp_raise_OSError(errno);
	for (code = 0; code <= KEY_MAX; code++) {
		if (is_normalized_key(code) && bit_is_set(keys, code)) {
			last_code = (uint16_t)code;
			return event_tuple((uint16_t)code, 1);
		}
	}
	if (last_code != 0) {
		uint16_t released = last_code;

		last_code = 0;
		return event_tuple(released, 0);
	}
	return mp_const_none;
}

static mp_obj_t keypad_read(void)
{
	if (keypad_fd < 0)
		mp_raise_OSError(ENODEV);
	for (;;) {
		struct input_event event;
		ssize_t count = read(keypad_fd, &event, sizeof(event));

		if (count < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return mp_const_none;
			if (errno == EINTR)
				continue;
			mp_raise_OSError(errno);
		}
		if (count != (ssize_t)sizeof(event))
			mp_raise_OSError(EIO);
		if (event.type == EV_SYN && event.code == SYN_DROPPED) {
			sync_dropped = true;
			continue;
		}
		if (sync_dropped) {
			if (event.type == EV_SYN && event.code == SYN_REPORT) {
				sync_dropped = false;
				return resync_state();
			}
			continue;
		}
		if (event.type != EV_KEY || !is_normalized_key(event.code))
			continue;
		if (event.value != 0)
			last_code = event.code;
		else if (last_code == event.code)
			last_code = 0;
		return event_tuple(event.code, event.value);
	}
}
static MP_DEFINE_CONST_FUN_OBJ_0(keypad_read_obj, keypad_read);

static const mp_rom_map_elem_t keypad_module_globals_table[] = {
	{ MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fplinux_keypad) },
	{ MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&keypad_open_obj) },
	{ MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&keypad_close_obj) },
	{ MP_ROM_QSTR(MP_QSTR_device), MP_ROM_PTR(&keypad_device_obj) },
	{ MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&keypad_read_obj) },
};
static MP_DEFINE_CONST_DICT(keypad_module_globals, keypad_module_globals_table);

const mp_obj_module_t fplinux_keypad_module = {
	.base = { &mp_type_module },
	.globals = (mp_obj_dict_t *)&keypad_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fplinux_keypad, fplinux_keypad_module);
