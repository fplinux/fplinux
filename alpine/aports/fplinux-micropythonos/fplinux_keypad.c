// SPDX-License-Identifier: MIT
/* FPLinux phone keypad and keyboard input module for MicroPython. */
/* fplinux-check: package-embedded */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "fplinux-input-session.h"
#include "fplinux-keyboard-text.h"
#include "fplinux-keypad.h"
#include "py/obj.h"
#include "py/runtime.h"

/* XKB keycodes are evdev codes offset by 8, as in the X11 protocol. */
#define FPLINUX_KEYPAD_XKB_KEYCODE_OFFSET 8U
#define FPLINUX_KEYPAD_TEXT_BYTES 64U
#define FPLINUX_KEYPAD_ERROR_BYTES 256U

struct keypad_event {
	enum fplinux_input_source source;
	unsigned int code;
	bool pressed;
	char text[FPLINUX_KEYPAD_TEXT_BYTES];
};

static struct fplinux_input_session input_session;
static bool input_open;
static struct xkb_context *text_context;
static struct xkb_keymap *text_keymap;
static struct xkb_state *text_state;
static struct keypad_event queued_event;
static bool event_queued;

static void close_keyboard_text(void)
{
	xkb_state_unref(text_state);
	xkb_keymap_unref(text_keymap);
	xkb_context_unref(text_context);
	text_state = NULL;
	text_keymap = NULL;
	text_context = NULL;
}

static bool open_keyboard_text(char *error, size_t error_size)
{
	struct fplinux_keyboard_text_layouts layouts;
	char keymap[FPLINUX_KEYBOARD_TEXT_KEYMAP_BYTES];

	if (!fplinux_keyboard_text_find_layouts(FPLINUX_KEYBOARD_TEXT_DATA_ROOT,
						&layouts, error, error_size))
		return false;
	if (!fplinux_keyboard_text_keymap(&layouts, keymap, sizeof(keymap))) {
		snprintf(error, error_size, "keyboard keymap is too long");
		return false;
	}
	/* The packaged data root is the only source of layout data. */
	text_context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES |
				       XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
	if (text_context == NULL ||
	    !xkb_context_include_path_append(text_context,
					     FPLINUX_KEYBOARD_TEXT_DATA_ROOT)) {
		snprintf(error, error_size,
			 "keyboard layout data %s is unavailable",
			 FPLINUX_KEYBOARD_TEXT_DATA_ROOT);
		close_keyboard_text();
		return false;
	}
	text_keymap = xkb_keymap_new_from_string(text_context, keymap,
						 XKB_KEYMAP_FORMAT_TEXT_V1,
						 XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (text_keymap != NULL)
		text_state = xkb_state_new(text_keymap);
	if (text_state == NULL) {
		snprintf(error, error_size,
			 "cannot compile the keyboard keymap");
		close_keyboard_text();
		return false;
	}
	return true;
}

static void close_input(void)
{
	if (input_open)
		fplinux_input_session_close(&input_session);
	input_open = false;
	event_queued = false;
	close_keyboard_text();
}

/*
 * The text is looked up before the key updates the state, so a modifier
 * applies to the following keys but not to itself.
 */
static void translate_keyboard_key(struct keypad_event *event)
{
	xkb_keycode_t keycode = event->code + FPLINUX_KEYPAD_XKB_KEYCODE_OFFSET;

	if (event->pressed) {
		int length = xkb_state_key_get_utf8(
			text_state, keycode, event->text, sizeof(event->text));

		if (length <= 0 || (size_t)length >= sizeof(event->text) ||
		    !fplinux_keyboard_text_is_printable(event->text))
			event->text[0] = '\0';
	}
	xkb_state_update_key(text_state, keycode,
			     event->pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
}

static bool fetch_event(struct keypad_event *event)
{
	struct fplinux_input_event input;

	while (fplinux_input_session_next(&input_session, &input)) {
		if (input.type != FPLINUX_INPUT_EVENT_KEY)
			continue;
		event->source = input.source;
		event->code = input.code;
		event->pressed = input.pressed;
		event->text[0] = '\0';
		if (input.source == FPLINUX_INPUT_SOURCE_KEYBOARD)
			translate_keyboard_key(event);
		return true;
	}
	return false;
}

static mp_obj_t keypad_open(void)
{
	char error[FPLINUX_KEYPAD_ERROR_BYTES];

	close_input();
	if (!open_keyboard_text(error, sizeof(error)))
		mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s"), error);
	if (!fplinux_input_session_open(
		    &input_session,
		    FPLINUX_INPUT_SOURCE_MASK(FPLINUX_INPUT_SOURCE_KEYPAD) |
			    FPLINUX_INPUT_SOURCE_MASK(
				    FPLINUX_INPUT_SOURCE_KEYBOARD),
		    error, sizeof(error))) {
		close_keyboard_text();
		mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s"), error);
	}
	input_open = true;
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(keypad_open_obj, keypad_open);

static mp_obj_t keypad_close(void)
{
	close_input();
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(keypad_close_obj, keypad_close);

static mp_obj_t event_tuple(const struct keypad_event *event)
{
	mp_obj_t values[4] = {
		MP_OBJ_NEW_SMALL_INT(event->source),
		mp_obj_new_int_from_uint(event->code),
		MP_OBJ_NEW_SMALL_INT(event->pressed ? 1 : 0),
		mp_obj_new_str(event->text, strlen(event->text)),
	};

	return mp_obj_new_tuple(4, values);
}

static mp_obj_t keypad_read(void)
{
	struct keypad_event event;

	if (!input_open)
		mp_raise_OSError(ENODEV);
	if (event_queued) {
		event = queued_event;
		event_queued = false;
	} else if (!fetch_event(&event)) {
		return mp_const_none;
	}
	return event_tuple(&event);
}
static MP_DEFINE_CONST_FUN_OBJ_0(keypad_read_obj, keypad_read);

static mp_obj_t keypad_pending(void)
{
	if (!input_open)
		mp_raise_OSError(ENODEV);
	if (!event_queued)
		event_queued = fetch_event(&queued_event);
	return mp_obj_new_bool(event_queued);
}
static MP_DEFINE_CONST_FUN_OBJ_0(keypad_pending_obj, keypad_pending);

static const mp_rom_map_elem_t keypad_module_globals_table[] = {
	{ MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fplinux_keypad) },
	{ MP_ROM_QSTR(MP_QSTR_KEYPAD),
	  MP_ROM_INT(FPLINUX_INPUT_SOURCE_KEYPAD) },
	{ MP_ROM_QSTR(MP_QSTR_KEYBOARD),
	  MP_ROM_INT(FPLINUX_INPUT_SOURCE_KEYBOARD) },
	/* Phone key codes; keyboard events keep their Linux key codes. */
	{ MP_ROM_QSTR(MP_QSTR_KEY_0), MP_ROM_INT(FPLINUX_KEY_0) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_1), MP_ROM_INT(FPLINUX_KEY_1) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_2), MP_ROM_INT(FPLINUX_KEY_2) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_3), MP_ROM_INT(FPLINUX_KEY_3) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_4), MP_ROM_INT(FPLINUX_KEY_4) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_5), MP_ROM_INT(FPLINUX_KEY_5) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_6), MP_ROM_INT(FPLINUX_KEY_6) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_7), MP_ROM_INT(FPLINUX_KEY_7) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_8), MP_ROM_INT(FPLINUX_KEY_8) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_9), MP_ROM_INT(FPLINUX_KEY_9) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_STAR), MP_ROM_INT(FPLINUX_KEY_STAR) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_POUND), MP_ROM_INT(FPLINUX_KEY_POUND) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_UP), MP_ROM_INT(FPLINUX_KEY_UP) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_DOWN), MP_ROM_INT(FPLINUX_KEY_DOWN) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_LEFT), MP_ROM_INT(FPLINUX_KEY_LEFT) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_RIGHT), MP_ROM_INT(FPLINUX_KEY_RIGHT) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_OK), MP_ROM_INT(FPLINUX_KEY_OK) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_SOFT_LEFT),
	  MP_ROM_INT(FPLINUX_KEY_SOFT_LEFT) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_SOFT_RIGHT),
	  MP_ROM_INT(FPLINUX_KEY_SOFT_RIGHT) },
	{ MP_ROM_QSTR(MP_QSTR_KEY_CALL), MP_ROM_INT(FPLINUX_KEY_CALL) },
	{ MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&keypad_open_obj) },
	{ MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&keypad_close_obj) },
	{ MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&keypad_read_obj) },
	{ MP_ROM_QSTR(MP_QSTR_pending), MP_ROM_PTR(&keypad_pending_obj) },
};
static MP_DEFINE_CONST_DICT(keypad_module_globals, keypad_module_globals_table);

const mp_obj_module_t fplinux_keypad_module = {
	.base = { &mp_type_module },
	.globals = (mp_obj_dict_t *)&keypad_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fplinux_keypad, fplinux_keypad_module);
