/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-keyboard.h"
#include "fplinux-keyboard-text.h"

#include <libtsm.h>
#include <stdio.h>
#include <string.h>

bool fplinux_terminal_keyboard_open(struct fplinux_terminal_keyboard *keyboard,
				    const char *data_root, char *error,
				    size_t error_size)
{
	struct fplinux_keyboard_text_layouts layouts;
	char keymap[FPLINUX_KEYBOARD_TEXT_KEYMAP_BYTES];

	memset(keyboard, 0, sizeof(*keyboard));
	if (!fplinux_keyboard_text_find_layouts(data_root, &layouts, error,
						error_size))
		return false;
	if (!fplinux_keyboard_text_keymap(&layouts, keymap, sizeof(keymap))) {
		snprintf(error, error_size, "cannot compose keyboard layouts");
		return false;
	}
	keyboard->context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
	if (!keyboard->context ||
	    !xkb_context_include_path_append(keyboard->context, data_root))
		goto fail;
	keyboard->keymap = xkb_keymap_new_from_string(
		keyboard->context, keymap, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keyboard->keymap)
		goto fail;
	keyboard->state = xkb_state_new(keyboard->keymap);
	if (!keyboard->state)
		goto fail;
	return true;
fail:
	snprintf(error, error_size, "cannot initialize XKB keyboard state");
	fplinux_terminal_keyboard_close(keyboard);
	return false;
}

void fplinux_terminal_keyboard_close(struct fplinux_terminal_keyboard *keyboard)
{
	xkb_state_unref(keyboard->state);
	xkb_keymap_unref(keyboard->keymap);
	xkb_context_unref(keyboard->context);
	memset(keyboard, 0, sizeof(*keyboard));
}

bool fplinux_terminal_keyboard_reset(struct fplinux_terminal_keyboard *keyboard)
{
	struct xkb_state *state = xkb_state_new(keyboard->keymap);

	if (!state)
		return false;
	xkb_state_unref(keyboard->state);
	keyboard->state = state;
	memset(keyboard->devices, 0, sizeof(keyboard->devices));
	memset(keyboard->references, 0, sizeof(keyboard->references));
	return true;
}

static struct fplinux_terminal_keyboard_device *
find_device(struct fplinux_terminal_keyboard *keyboard, uint64_t id,
	    bool create)
{
	struct fplinux_terminal_keyboard_device *empty = NULL;
	unsigned int index;

	if (!id)
		return NULL;
	for (index = 0; index < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++index) {
		struct fplinux_terminal_keyboard_device *device =
			&keyboard->devices[index];

		if (device->id == id)
			return device;
		if (!device->id)
			empty = device;
	}
	if (create && empty) {
		empty->id = id;
		return empty;
	}
	return NULL;
}

static bool translate(struct fplinux_terminal_keyboard *keyboard,
		      unsigned int code, struct fplinux_terminal_key *key)
{
	const xkb_keysym_t *plain;
	xkb_keycode_t keycode = code + 8;
	int count;

	memset(key, 0, sizeof(*key));
	key->keysym = xkb_state_key_get_one_sym(keyboard->state, keycode);
	switch (key->keysym) {
	case XKB_KEY_NoSymbol:
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Num_Lock:
	case XKB_KEY_ISO_Level3_Shift:
	case XKB_KEY_ISO_Next_Group:
	case XKB_KEY_ISO_Prev_Group:
		return false;
	}
	key->unicode = xkb_keysym_to_utf32(key->keysym);
	if (!key->unicode)
		key->unicode = TSM_VTE_INVALID;
	key->ascii = TSM_VTE_INVALID;
	count = xkb_keymap_key_get_syms_by_level(keyboard->keymap, keycode, 0,
						 0, &plain);
	if (count == 1 && plain[0] < 128)
		key->ascii = plain[0];
	if (xkb_state_mod_name_is_active(keyboard->state, XKB_MOD_NAME_SHIFT,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		key->modifiers |= TSM_SHIFT_MASK;
	if (xkb_state_mod_name_is_active(keyboard->state, XKB_MOD_NAME_CTRL,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		key->modifiers |= TSM_CONTROL_MASK;
	if (xkb_state_mod_name_is_active(keyboard->state, XKB_MOD_NAME_ALT,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		key->modifiers |= TSM_ALT_MASK;
	if (xkb_state_mod_name_is_active(keyboard->state, XKB_MOD_NAME_CAPS,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		key->modifiers |= TSM_LOCK_MASK;
	key->repeats = xkb_keymap_key_repeats(keyboard->keymap, keycode);
	return true;
}

bool fplinux_terminal_keyboard_event(struct fplinux_terminal_keyboard *keyboard,
				     const struct fplinux_input_event *event,
				     struct fplinux_terminal_key *key)
{
	struct fplinux_terminal_keyboard_device *device;
	unsigned int code = event->code;
	unsigned int index;

	if (event->source != FPLINUX_INPUT_SOURCE_KEYBOARD)
		return false;
	device = find_device(keyboard, event->device_id,
			     event->type != FPLINUX_INPUT_EVENT_DEVICE_REMOVED);
	if (!device)
		return false;
	if (event->type == FPLINUX_INPUT_EVENT_DEVICE_REMOVED) {
		for (index = 0; index < KEY_CNT; ++index)
			if (device->pressed[index] &&
			    --keyboard->references[index] == 0)
				xkb_state_update_key(keyboard->state, index + 8,
						     XKB_KEY_UP);
		memset(device, 0, sizeof(*device));
		return false;
	}
	if (event->type != FPLINUX_INPUT_EVENT_KEY || code >= KEY_CNT ||
	    device->pressed[code] == event->pressed)
		return false;
	device->pressed[code] = event->pressed;
	if (event->pressed) {
		if (keyboard->references[code]++ == 0)
			xkb_state_update_key(keyboard->state, code + 8,
					     XKB_KEY_DOWN);
		return translate(keyboard, code, key);
	}
	if (--keyboard->references[code] == 0)
		xkb_state_update_key(keyboard->state, code + 8, XKB_KEY_UP);
	return false;
}

bool fplinux_terminal_keyboard_repeat(struct fplinux_terminal_keyboard *keyboard,
				      uint64_t device_id, unsigned int code,
				      struct fplinux_terminal_key *key)
{
	struct fplinux_terminal_keyboard_device *device =
		find_device(keyboard, device_id, false);

	return device && code < KEY_CNT && device->pressed[code] &&
	       translate(keyboard, code, key) && key->repeats;
}
