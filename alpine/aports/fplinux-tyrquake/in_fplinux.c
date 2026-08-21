// SPDX-License-Identifier: GPL-2.0-or-later
/* Native evdev input backend for FPLinux. */
/* fplinux-check: package-embedded */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "common.h"
#include "console.h"
#include "fplinux-device.h"
#include "input.h"
#include "keys.h"
#include "quakedef.h"
#include "sys.h"

#define FPLINUX_QUAKE_INPUT_EVENT_COUNT 32
#define FPLINUX_QUAKE_INPUT_PATH_BYTES 32
#define FPLINUX_QUAKE_INPUT_NAME_BYTES 128
#define FPLINUX_QUAKE_INPUT_PHYS_BYTES 128
#define BITS_PER_LONG (8U * sizeof(unsigned long))
#define NBITS(max) (((max) + BITS_PER_LONG) / BITS_PER_LONG)

enum input_role {
	FPLINUX_QUAKE_INPUT_ROLE_PHONE,
	FPLINUX_QUAKE_INPUT_ROLE_HOST,
};

enum input_mode {
	FPLINUX_QUAKE_INPUT_MODE_PHONE,
	FPLINUX_QUAKE_INPUT_MODE_HOST,
};

struct input_device {
	int fd;
	enum input_role role;
	qboolean active;
	qboolean grabbed;
	qboolean sync_lost;
	unsigned long key_state[NBITS(KEY_MAX)];
	char path[FPLINUX_QUAKE_INPUT_PATH_BYTES];
	char name[FPLINUX_QUAKE_INPUT_NAME_BYTES];
};

static struct input_device input_devices[FPLINUX_QUAKE_INPUT_EVENT_COUNT];
static unsigned int input_device_count;
static enum input_mode input_mode;

static cvar_t m_filter = {
	.name = "m_filter",
	.string = "0",
	.flags = CVAR_CONFIG,
};

cvar_t _windowed_mouse = {
	.name = "_windowed_mouse",
	.string = "0",
	.flags = CVAR_CONFIG,
};

static qboolean bit_is_set(unsigned int bit, const unsigned long *bits)
{
	return !!(bits[bit / BITS_PER_LONG] & (1UL << (bit % BITS_PER_LONG)));
}

static void set_bit_value(unsigned int bit, unsigned long *bits, qboolean value)
{
	unsigned long mask = 1UL << (bit % BITS_PER_LONG);
	unsigned long *word = &bits[bit / BITS_PER_LONG];

	if (value)
		*word |= mask;
	else
		*word &= ~mask;
}

static const char *role_name(enum input_role role)
{
	return role == FPLINUX_QUAKE_INPUT_ROLE_PHONE ? "phone" : "keyboard";
}

static void parse_input_mode(void)
{
	const char *value = NULL;
	int matches = 0;
	int i;

	for (i = 1; i < com_argc; ++i) {
		if (strcmp(com_argv[i], "-input"))
			continue;
		++matches;
		if (i + 1 < com_argc)
			value = com_argv[i + 1];
	}

	if (matches != 1 || !value)
		Sys_Error(
			"FPLinux input: require exactly one -input phone|keyboard");
	if (!strcmp(value, "phone"))
		input_mode = FPLINUX_QUAKE_INPUT_MODE_PHONE;
	else if (!strcmp(value, "keyboard"))
		input_mode = FPLINUX_QUAKE_INPUT_MODE_HOST;
	else
		Sys_Error("FPLinux input: unsupported mode '%s'", value);
}

static qboolean read_capabilities(int fd, const char *path,
				  unsigned long *event_bits,
				  unsigned long *key_bits)
{
	if (ioctl(fd, EVIOCGBIT(0, NBITS(EV_MAX) * sizeof(unsigned long)),
		  event_bits) < 0) {
		Con_Printf("FPLinux input: EVIOCGBIT(%s): %s\n", path,
			   strerror(errno));
		return false;
	}
	if (!bit_is_set(EV_KEY, event_bits) ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, NBITS(KEY_MAX) * sizeof(unsigned long)),
		  key_bits) < 0) {
		Con_Printf("FPLinux input: EV_KEY capabilities (%s): %s\n",
			   path, strerror(errno));
		return false;
	}
	return true;
}

static qboolean has_required_keys(const unsigned long *key_bits,
				  const unsigned int *keys, size_t key_count)
{
	size_t i;

	for (i = 0; i < key_count; ++i)
		if (!bit_is_set(keys[i], key_bits))
			return false;
	return true;
}

static qboolean validate_phone(int fd, const char *path)
{
	static const unsigned int required_keys[] = {
		KEY_0,	   KEY_1,	  KEY_2,	  KEY_3,     KEY_4,
		KEY_5,	   KEY_6,	  KEY_7,	  KEY_8,     KEY_9,
		KEY_ENTER, KEY_BACKSPACE, KEY_TAB,	  KEY_UP,    KEY_DOWN,
		KEY_LEFT,  KEY_RIGHT,	  KEY_KPASTERISK, KEY_KPDOT,
	};
	unsigned long event_bits[NBITS(EV_MAX)] = { 0 };
	unsigned long key_bits[NBITS(KEY_MAX)] = { 0 };

	if (!read_capabilities(fd, path, event_bits, key_bits))
		return false;
	return has_required_keys(key_bits, required_keys,
				 sizeof(required_keys) /
					 sizeof(required_keys[0]));
}

static qboolean validate_host_keyboard(int fd, const char *path)
{
	static const unsigned int required_keys[] = {
		KEY_A,	KEY_Z,	  KEY_ENTER, KEY_SPACE, KEY_ESC,
		KEY_UP, KEY_DOWN, KEY_LEFT,  KEY_RIGHT,
	};
	unsigned long event_bits[NBITS(EV_MAX)] = { 0 };
	unsigned long key_bits[NBITS(KEY_MAX)] = { 0 };

	if (!read_capabilities(fd, path, event_bits, key_bits))
		return false;
	return has_required_keys(key_bits, required_keys,
				 sizeof(required_keys) /
					 sizeof(required_keys[0]));
}

static qboolean is_fplinux_host_keyboard(const struct input_id *id)
{
	return id->bustype == BUS_VIRTUAL &&
	       id->vendor == FPLINUX_INPUT_HOST_VENDOR_ID &&
	       id->product == FPLINUX_INPUT_HOST_PRODUCT_ID;
}

static qboolean is_fplinux_phone_keypad(int fd)
{
	char phys[FPLINUX_QUAKE_INPUT_PHYS_BYTES];

	if (ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys) < 0)
		return false;
	phys[sizeof(phys) - 1] = '\0';
	return strcmp(phys, FPLINUX_INPUT_PHONE_PHYS) == 0;
}

static qboolean grab_device(int fd, const char *path, const char *name)
{
	int attempts = 20;

	while (ioctl(fd, EVIOCGRAB, 1) < 0) {
		int saved_errno = errno;

		if (saved_errno != EBUSY || --attempts == 0) {
			Con_Printf("FPLinux input: cannot grab %s (%s): %s\n",
				   path, name, strerror(saved_errno));
			return false;
		}
		usleep(50000);
	}
	return true;
}

static void register_input_device(const char *path)
{
	struct input_device *device;
	struct input_id id;
	enum input_role role;
	char name[FPLINUX_QUAKE_INPUT_NAME_BYTES];
	int fd;

	if (input_device_count >= FPLINUX_QUAKE_INPUT_EVENT_COUNT)
		return;

	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	if (ioctl(fd, EVIOCGID, &id) < 0 ||
	    ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
		close(fd);
		return;
	}
	name[sizeof(name) - 1] = '\0';

	if (is_fplinux_host_keyboard(&id)) {
		role = FPLINUX_QUAKE_INPUT_ROLE_HOST;
		if (!validate_host_keyboard(fd, path)) {
			close(fd);
			Sys_Error("FPLinux input: invalid host keyboard at %s",
				  path);
		}
	} else if (is_fplinux_phone_keypad(fd)) {
		role = FPLINUX_QUAKE_INPUT_ROLE_PHONE;
		if (!validate_phone(fd, path)) {
			close(fd);
			Sys_Error("FPLinux input: invalid phone keypad at %s",
				  path);
		}
	} else {
		close(fd);
		return;
	}

	if (!grab_device(fd, path, name)) {
		close(fd);
		Sys_Error("FPLinux input: %s is not available", name);
	}

	device = &input_devices[input_device_count++];
	memset(device, 0, sizeof(*device));
	device->fd = fd;
	device->role = role;
	device->grabbed = true;
	snprintf(device->path, sizeof(device->path), "%s", path);
	snprintf(device->name, sizeof(device->name), "%s", name);
}

static unsigned int count_role(enum input_role role)
{
	unsigned int count = 0;
	unsigned int i;

	for (i = 0; i < input_device_count; ++i)
		if (input_devices[i].fd >= 0 && input_devices[i].role == role)
			++count;
	return count;
}

static void close_input_device(struct input_device *device)
{
	if (device->fd < 0)
		return;
	if (device->grabbed && ioctl(device->fd, EVIOCGRAB, 0) < 0 &&
	    errno != ENODEV)
		Con_Printf("FPLinux input: release %s failed: %s\n",
			   device->path, strerror(errno));
	close(device->fd);
	device->fd = -1;
	device->grabbed = false;
	device->active = false;
	memset(device->key_state, 0, sizeof(device->key_state));
}

static void close_input_devices(void)
{
	unsigned int i;

	for (i = 0; i < input_device_count; ++i)
		close_input_device(&input_devices[i]);
	input_device_count = 0;
}

static void scan_input_devices(void)
{
	enum input_role selected_role;
	unsigned int phone_count;
	unsigned int host_count;
	unsigned int i;

	for (i = 0; i < FPLINUX_QUAKE_INPUT_EVENT_COUNT; ++i) {
		char path[FPLINUX_QUAKE_INPUT_PATH_BYTES];

		snprintf(path, sizeof(path), "/dev/input/event%u", i);
		register_input_device(path);
	}

	phone_count = count_role(FPLINUX_QUAKE_INPUT_ROLE_PHONE);
	host_count = count_role(FPLINUX_QUAKE_INPUT_ROLE_HOST);
	if (phone_count > 1 || host_count > 1) {
		close_input_devices();
		Sys_Error(
			"FPLinux input: ambiguous devices: phone=%u keyboard=%u",
			phone_count, host_count);
	}

	selected_role = input_mode == FPLINUX_QUAKE_INPUT_MODE_PHONE ?
				FPLINUX_QUAKE_INPUT_ROLE_PHONE :
				FPLINUX_QUAKE_INPUT_ROLE_HOST;
	if (count_role(selected_role) != 1) {
		const char *selected_name = role_name(selected_role);

		close_input_devices();
		Sys_Error("FPLinux input: selected %s device is unavailable",
			  selected_name);
	}

	for (i = 0; i < input_device_count; ++i) {
		struct input_device *device = &input_devices[i];

		device->active = device->role == selected_role;
		Con_Printf("FPLinux input: grabbed %s (%s) as %s\n",
			   device->path, device->name,
			   device->active ? "active" : "quarantined");
	}
}

static knum_t translate_phone(unsigned int code)
{
	switch (code) {
	case KEY_1:
		return K_1;
	case KEY_2:
		return K_2;
	case KEY_3:
		return K_3;
	case KEY_4:
		return K_4;
	case KEY_5:
		return K_5;
	case KEY_6:
		return K_6;
	case KEY_7:
		return K_7;
	case KEY_8:
		return K_8;
	case KEY_9:
		return K_9;
	case KEY_0:
		return K_LCTRL;
	case KEY_KPASTERISK:
		return K_SPACE;
	case KEY_KPDOT:
		return K_HASH;
	case KEY_ENTER:
		return K_ENTER;
	case KEY_BACKSPACE:
		return K_ESCAPE;
	case KEY_TAB:
		return K_TAB;
	case KEY_UP:
		return K_LEFTARROW;
	case KEY_DOWN:
		return K_RIGHTARROW;
	case KEY_LEFT:
		return K_DOWNARROW;
	case KEY_RIGHT:
		return K_UPARROW;
	default:
		return K_UNKNOWN;
	}
}

static knum_t translate_keyboard(unsigned int code)
{
	static const knum_t number_row[] = {
		K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_0,
	};
	static const knum_t qwerty_row[] = {
		K_q, K_w, K_e, K_r, K_t, K_y, K_u, K_i, K_o, K_p,
	};
	static const knum_t home_row[] = {
		K_a, K_s, K_d, K_f, K_g, K_h, K_j, K_k, K_l,
	};
	static const knum_t bottom_row[] = {
		K_z, K_x, K_c, K_v, K_b, K_n, K_m,
	};

	if (code >= KEY_1 && code <= KEY_0)
		return number_row[code - KEY_1];
	if (code >= KEY_Q && code <= KEY_P)
		return qwerty_row[code - KEY_Q];
	if (code >= KEY_A && code <= KEY_L)
		return home_row[code - KEY_A];
	if (code >= KEY_Z && code <= KEY_M)
		return bottom_row[code - KEY_Z];
	if (code >= KEY_F1 && code <= KEY_F10)
		return (knum_t)(K_F1 + code - KEY_F1);

	switch (code) {
	case KEY_ESC:
		return K_ESCAPE;
	case KEY_MINUS:
		return K_MINUS;
	case KEY_EQUAL:
		return K_EQUALS;
	case KEY_BACKSPACE:
		return K_BACKSPACE;
	case KEY_TAB:
		return K_TAB;
	case KEY_LEFTBRACE:
		return K_LEFTBRACKET;
	case KEY_RIGHTBRACE:
		return K_RIGHTBRACKET;
	case KEY_ENTER:
		return K_ENTER;
	case KEY_SEMICOLON:
		return K_SEMICOLON;
	case KEY_APOSTROPHE:
		return K_QUOTE;
	case KEY_GRAVE:
		return K_BACKQUOTE;
	case KEY_BACKSLASH:
		return K_BACKSLASH;
	case KEY_COMMA:
		return K_COMMA;
	case KEY_DOT:
		return K_PERIOD;
	case KEY_SLASH:
		return K_SLASH;
	case KEY_SPACE:
		return K_SPACE;
	case KEY_DELETE:
		return K_DEL;
	case KEY_INSERT:
		return K_INS;
	case KEY_HOME:
		return K_HOME;
	case KEY_END:
		return K_END;
	case KEY_PAGEUP:
		return K_PGUP;
	case KEY_PAGEDOWN:
		return K_PGDN;
	case KEY_UP:
		return K_UPARROW;
	case KEY_DOWN:
		return K_DOWNARROW;
	case KEY_LEFT:
		return K_LEFTARROW;
	case KEY_RIGHT:
		return K_RIGHTARROW;
	case KEY_PAUSE:
		return K_PAUSE;
	case KEY_F11:
		return K_F11;
	case KEY_F12:
		return K_F12;
	case KEY_LEFTSHIFT:
		return K_LSHIFT;
	case KEY_RIGHTSHIFT:
		return K_RSHIFT;
	case KEY_LEFTCTRL:
		return K_LCTRL;
	case KEY_RIGHTCTRL:
		return K_RCTRL;
	case KEY_LEFTALT:
		return K_LALT;
	case KEY_RIGHTALT:
		return K_RALT;
	case KEY_LEFTMETA:
		return K_LSUPER;
	case KEY_RIGHTMETA:
		return K_RSUPER;
	case KEY_CAPSLOCK:
		return K_CAPSLOCK;
	case KEY_NUMLOCK:
		return K_NUMLOCK;
	case KEY_SCROLLLOCK:
		return K_SCROLLOCK;
	case KEY_SYSRQ:
		return K_SYSREQ;
	case KEY_MENU:
		return K_MENU;
	case KEY_KP0:
		return K_KP0;
	case KEY_KP1:
		return K_KP1;
	case KEY_KP2:
		return K_KP2;
	case KEY_KP3:
		return K_KP3;
	case KEY_KP4:
		return K_KP4;
	case KEY_KP5:
		return K_KP5;
	case KEY_KP6:
		return K_KP6;
	case KEY_KP7:
		return K_KP7;
	case KEY_KP8:
		return K_KP8;
	case KEY_KP9:
		return K_KP9;
	case KEY_KPDOT:
		return K_KP_PERIOD;
	case KEY_KPSLASH:
		return K_KP_DIVIDE;
	case KEY_KPASTERISK:
		return K_KP_MULTIPLY;
	case KEY_KPMINUS:
		return K_KP_MINUS;
	case KEY_KPPLUS:
		return K_KP_PLUS;
	case KEY_KPENTER:
		return K_KP_ENTER;
	case KEY_KPEQUAL:
		return K_KP_EQUALS;
	default:
		return K_UNKNOWN;
	}
}

static knum_t translate_key(const struct input_device *device,
			    unsigned int code)
{
	if (device->role == FPLINUX_QUAKE_INPUT_ROLE_PHONE)
		return translate_phone(code);
	return translate_keyboard(code);
}

static void release_device_keys(struct input_device *device)
{
	unsigned int code;

	if (!device->active)
		return;
	for (code = 0; code <= KEY_MAX; ++code) {
		knum_t key;

		if (!bit_is_set(code, device->key_state))
			continue;
		key = translate_key(device, code);
		if (key != K_UNKNOWN)
			Key_Event(key, false);
	}
	memset(device->key_state, 0, sizeof(device->key_state));
}

static void handle_key_event(struct input_device *device, unsigned int code,
			     int value)
{
	knum_t key;

	if (!device->active || code > KEY_MAX)
		return;
	key = translate_key(device, code);
	if (key == K_UNKNOWN)
		return;
	set_bit_value(code, device->key_state, value != 0);
	Key_Event(key, value != 0);
}

static void resync_keys(struct input_device *device)
{
	unsigned long current[NBITS(KEY_MAX)] = { 0 };
	unsigned int code;

	if (!device->active)
		return;
	if (ioctl(device->fd, EVIOCGKEY(sizeof(current)), current) < 0) {
		release_device_keys(device);
		Sys_Error("FPLinux input: cannot resync %s: %s", device->path,
			  strerror(errno));
	}

	for (code = 0; code <= KEY_MAX; ++code) {
		qboolean was_down = bit_is_set(code, device->key_state);
		qboolean is_down = bit_is_set(code, current);
		knum_t key;

		if (was_down == is_down)
			continue;
		key = translate_key(device, code);
		set_bit_value(code, device->key_state, is_down);
		if (key != K_UNKNOWN)
			Key_Event(key, is_down);
	}
}

static void handle_input_event(struct input_device *device,
			       const struct input_event *event)
{
	if (device->sync_lost) {
		if (event->type == EV_SYN && event->code == SYN_REPORT) {
			device->sync_lost = false;
			resync_keys(device);
		}
		return;
	}
	if (event->type == EV_SYN && event->code == SYN_DROPPED) {
		device->sync_lost = true;
		return;
	}
	if (event->type == EV_KEY)
		handle_key_event(device, event->code, event->value);
}

static void read_input_device(struct input_device *device)
{
	struct input_event events[16];
	ssize_t size;

	while ((size = read(device->fd, events, sizeof(events))) > 0) {
		size_t count;
		size_t i;

		if ((size_t)size % sizeof(events[0])) {
			release_device_keys(device);
			Sys_Error("FPLinux input: short event record from %s",
				  device->path);
		}
		count = (size_t)size / sizeof(events[0]);
		for (i = 0; i < count; ++i)
			handle_input_event(device, &events[i]);
	}

	if (size == 0 || (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
			  errno != EINTR)) {
		qboolean was_active = device->active;
		char name[FPLINUX_QUAKE_INPUT_NAME_BYTES];

		snprintf(name, sizeof(name), "%s", device->name);
		release_device_keys(device);
		close_input_device(device);
		if (was_active)
			Sys_Error(
				"FPLinux input: active %s device disconnected",
				name);
		Con_Printf("FPLinux input: quarantined %s disconnected\n",
			   name);
	}
}

void IN_AddCommands(void)
{
}

void IN_RegisterVariables(void)
{
	Cvar_RegisterVariable(&m_filter);
	Cvar_RegisterVariable(&_windowed_mouse);
}

void IN_Init(void)
{
	parse_input_mode();
	scan_input_devices();
}

void IN_Shutdown(void)
{
	Key_ClearAllStates();
	close_input_devices();
}

void IN_Commands(void)
{
	unsigned int i;

	for (i = 0; i < input_device_count; ++i)
		if (input_devices[i].fd >= 0)
			read_input_device(&input_devices[i]);
}

void IN_Move(usercmd_t *cmd)
{
	(void)cmd;
}

void IN_Accumulate(void)
{
}

void IN_ModeChanged(void)
{
}

void IN_ClearStates(void)
{
	unsigned int i;

	Key_ClearAllStates();
	for (i = 0; i < input_device_count; ++i)
		memset(input_devices[i].key_state, 0,
		       sizeof(input_devices[i].key_state));
}

void IN_SetFocus(qboolean focus)
{
	if (!focus)
		IN_ClearStates();
}

qboolean IN_HaveFocus(void)
{
	return true;
}
