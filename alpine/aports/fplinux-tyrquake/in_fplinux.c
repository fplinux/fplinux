// SPDX-License-Identifier: GPL-2.0-or-later
/* Native input backend for FPLinux. */
/* fplinux-check: package-embedded */

#include <linux/input.h>
#include <string.h>

#include "client.h"
#include "common.h"
#include "console.h"
#include "fplinux-drm-session.h"
#include "fplinux-input-session.h"
#include "fplinux-keypad.h"
#include "fplinux-quake-internal.h"
#include "input.h"
#include "keys.h"
#include "quakedef.h"
#include "sys.h"

#define FPLINUX_QUAKE_INPUT_ERROR_BYTES 128
#define FPLINUX_QUAKE_INPUT_WHEEL_LIMIT 16
#define FPLINUX_QUAKE_INPUT_KEY_BYTES ((KEY_CNT + 7U) / 8U)

struct held_device {
	uint64_t id;
	unsigned char keys[FPLINUX_QUAKE_INPUT_KEY_BYTES];
};

static struct fplinux_input_session input_session;
static qboolean input_session_open;
static qboolean input_active;
static struct held_device held_devices[FPLINUX_INPUT_SESSION_DEVICE_COUNT];
static unsigned int held_actions[K_LAST];

static float mouse_dx, mouse_dy;
static float old_mouse_dx, old_mouse_dy;

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

static void clear_mouse_motion(void)
{
	mouse_dx = 0.0f;
	mouse_dy = 0.0f;
	old_mouse_dx = 0.0f;
	old_mouse_dy = 0.0f;
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

static knum_t translate_keypad(unsigned int code)
{
	switch (code) {
	case FPLINUX_KEY_0:
		return K_LCTRL;
	case FPLINUX_KEY_1:
		return K_a;
	case FPLINUX_KEY_2:
		return K_LEFTARROW;
	case FPLINUX_KEY_3:
		return K_d;
	case FPLINUX_KEY_4:
		return K_s;
	case FPLINUX_KEY_5:
		return K_RIGHTARROW;
	case FPLINUX_KEY_6:
		return K_w;
	case FPLINUX_KEY_7:
		return K_LEFTBRACKET;
	case FPLINUX_KEY_8:
		return K_TAB;
	case FPLINUX_KEY_9:
		return K_RIGHTBRACKET;
	case FPLINUX_KEY_STAR:
	case FPLINUX_KEY_SOFT_LEFT:
		return K_SPACE;
	case FPLINUX_KEY_POUND:
		return K_KP_PERIOD;
	case FPLINUX_KEY_UP:
		return K_LEFTARROW;
	case FPLINUX_KEY_DOWN:
		return K_RIGHTARROW;
	case FPLINUX_KEY_LEFT:
		return K_DOWNARROW;
	case FPLINUX_KEY_RIGHT:
		return K_UPARROW;
	case FPLINUX_KEY_OK:
	case FPLINUX_KEY_CALL:
		return K_ENTER;
	case FPLINUX_KEY_SOFT_RIGHT:
		return K_ESCAPE;
	default:
		return K_UNKNOWN;
	}
}

/* K_MOUSE4 and K_MOUSE5 spell the wheel, so side buttons stay unmapped. */
static knum_t translate_mouse(unsigned int code)
{
	switch (code) {
	case BTN_LEFT:
		return K_MOUSE1;
	case BTN_RIGHT:
		return K_MOUSE2;
	case BTN_MIDDLE:
		return K_MOUSE3;
	default:
		return K_UNKNOWN;
	}
}

static struct held_device *find_held_device(uint64_t id)
{
	size_t i;

	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i)
		if (held_devices[i].id == id)
			return &held_devices[i];
	return NULL;
}

static void clear_key_states(void)
{
	size_t i;

	Key_ClearAllStates();
	memset(held_actions, 0, sizeof(held_actions));
	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i)
		memset(held_devices[i].keys, 0, sizeof(held_devices[i].keys));
}

static void handle_key(const struct fplinux_input_event *event,
		       knum_t (*translate)(unsigned int code))
{
	struct held_device *device = find_held_device(event->device_id);
	knum_t key = translate(event->code);
	unsigned char *byte;
	unsigned int mask;

	if (!device || key == K_UNKNOWN || event->code >= KEY_CNT)
		return;
	byte = &device->keys[event->code / 8U];
	mask = 1U << (event->code % 8U);
	if (!!(*byte & mask) == event->pressed)
		return;
	if (event->pressed) {
		*byte |= mask;
		if (++held_actions[key] == 1U)
			Key_Event(key, true);
	} else {
		*byte &= ~mask;
		if (--held_actions[key] == 0U)
			Key_Event(key, false);
	}
}

static void handle_wheel(int clicks)
{
	knum_t key = clicks > 0 ? K_MWHEELUP : K_MWHEELDOWN;
	int count = clicks > 0 ? clicks : -clicks;

	if (count > FPLINUX_QUAKE_INPUT_WHEEL_LIMIT)
		count = FPLINUX_QUAKE_INPUT_WHEEL_LIMIT;
	while (count-- > 0) {
		Key_Event(key, true);
		Key_Event(key, false);
	}
}

static void handle_event(const struct fplinux_input_event *event)
{
	struct held_device *device;

	switch (event->type) {
	case FPLINUX_INPUT_EVENT_KEY:
		handle_key(event, event->source == FPLINUX_INPUT_SOURCE_KEYPAD ?
					  translate_keypad :
					  translate_keyboard);
		break;
	case FPLINUX_INPUT_EVENT_BUTTON:
		handle_key(event, translate_mouse);
		break;
	case FPLINUX_INPUT_EVENT_MOTION:
		mouse_dx += (float)event->dx;
		mouse_dy += (float)event->dy;
		break;
	case FPLINUX_INPUT_EVENT_WHEEL:
		handle_wheel(event->wheel_clicks);
		break;
	case FPLINUX_INPUT_EVENT_DEVICE_ADDED:
		device = find_held_device(0);
		if (device)
			device->id = event->device_id;
		Con_Printf("FPLinux input: %s connected\n", event->name);
		break;
	case FPLINUX_INPUT_EVENT_DEVICE_REMOVED:
		device = find_held_device(event->device_id);
		if (device)
			memset(device, 0, sizeof(*device));
		if (event->source == FPLINUX_INPUT_SOURCE_POINTER)
			clear_mouse_motion();
		Con_Printf("FPLinux input: %s disconnected\n", event->name);
		break;
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

static bool input_active_changed(struct fplinux_drm_session *display,
				 bool active, void *data)
{
	struct fplinux_input_event event;
	char error[FPLINUX_QUAKE_INPUT_ERROR_BYTES];

	(void)display;
	(void)data;
	input_active = active;
	if (!input_session_open)
		return true;
	if (!active) {
		fplinux_input_session_suspend(&input_session);
		while (fplinux_input_session_next(&input_session, &event))
			if (event.type == FPLINUX_INPUT_EVENT_DEVICE_ADDED ||
			    event.type == FPLINUX_INPUT_EVENT_DEVICE_REMOVED)
				handle_event(&event);
		clear_key_states();
		clear_mouse_motion();
		return true;
	}
	if (!fplinux_input_session_resume(&input_session, error,
					  sizeof(error))) {
		Con_Printf("FPLinux input: %s\n", error);
		return false;
	}
	return true;
}

void IN_Init(void)
{
	char error[FPLINUX_QUAKE_INPUT_ERROR_BYTES];

	if (!fplinux_input_session_open(
		    &input_session,
		    FPLINUX_INPUT_SOURCE_MASK(FPLINUX_INPUT_SOURCE_KEYPAD) |
			    FPLINUX_INPUT_SOURCE_MASK(
				    FPLINUX_INPUT_SOURCE_KEYBOARD) |
			    FPLINUX_INPUT_SOURCE_MASK(
				    FPLINUX_INPUT_SOURCE_POINTER),
		    error, sizeof(error)))
		Sys_Error("FPLinux input: %s", error);
	input_session_open = true;
	if (!fplinux_drm_session_set_active_handler(
		    fplinux_quake_display_session(), input_active_changed,
		    NULL))
		Sys_Error("FPLinux input: cannot claim active display input");
}

void IN_Shutdown(void)
{
	clear_key_states();
	if (input_session_open) {
		fplinux_drm_session_set_active_handler(
			fplinux_quake_display_session(), NULL, NULL);
		fplinux_input_session_close(&input_session);
	}
	input_session_open = false;
	memset(held_devices, 0, sizeof(held_devices));
}

void IN_Commands(void)
{
	struct fplinux_input_event event;

	if (!input_session_open)
		return;
	while (fplinux_input_session_next(&input_session, &event))
		handle_event(&event);
}

void IN_Move(usercmd_t *cmd)
{
	float x;
	float y;

	x = mouse_dx;
	y = mouse_dy;
	mouse_dx = 0;
	mouse_dy = 0;
	if (m_filter.value) {
		x = (x + old_mouse_dx) * 0.5f;
		y = (y + old_mouse_dy) * 0.5f;
	}
	old_mouse_dx = x;
	old_mouse_dy = y;
	if (x == 0.0f && y == 0.0f)
		return;

	x *= sensitivity.value;
	y *= sensitivity.value;

	/*
	 * The mouse always looks.  The engine's own free-look state is a
	 * toggle meant for a mouse that also drives forward motion.
	 */
	if (in_strafe.state & 1) {
		cmd->sidemove += m_side.value * x;
		cmd->forwardmove -= m_forward.value * y;
		return;
	}

	cl.viewangles[YAW] -= m_yaw.value * x;
	V_StopPitchDrift();
	cl.viewangles[PITCH] += m_pitch.value * y;
	if (cl.viewangles[PITCH] > cl_maxpitch.value)
		cl.viewangles[PITCH] = cl_maxpitch.value;
	if (cl.viewangles[PITCH] < cl_minpitch.value)
		cl.viewangles[PITCH] = cl_minpitch.value;
}

void IN_Accumulate(void)
{
}

void IN_ModeChanged(void)
{
}

void IN_ClearStates(void)
{
	clear_key_states();
	clear_mouse_motion();
}

void IN_SetFocus(qboolean focus)
{
	if (!input_active_changed(fplinux_quake_display_session(), focus, NULL))
		Sys_Error("FPLinux input: cannot regain input focus");
}

qboolean IN_HaveFocus(void)
{
	return input_active;
}
