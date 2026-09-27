/* SPDX-License-Identifier: GPL-2.0-only */
/* Exercise the native module using fake runtime, XKB and session boundaries. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "fplinux-input-session.h"
#include "fplinux-keyboard-text.h"
#include "fplinux-keypad-internal.h"
#include "py/obj.h"

extern const mp_obj_module_t fplinux_keypad_module;
const char mp_type_module;
const char mp_type_OSError;

struct xkb_context {
	int unused;
};
struct xkb_keymap {
	int unused;
};
struct xkb_state {
	bool shift;
};

static struct xkb_context text_context;
static struct xkb_keymap text_keymap;
static struct xkb_state text_state;
static struct fplinux_input_event queue[64];
static size_t queue_head, queue_tail;
static bool suspended;

static mp_obj_t call(const char *name)
{
	const mp_obj_dict_t *dictionary = fplinux_keypad_module.globals;
	size_t i;

	for (i = 0; i < dictionary->count; ++i)
		if (!strcmp(dictionary->table[i].key, name)) {
			const struct test_mp_function *function =
				dictionary->table[i].value;

			return function->call();
		}
	abort();
}

static void event(enum fplinux_input_event_type type, uint64_t id,
		  enum fplinux_input_source source, unsigned int code,
		  bool pressed)
{
	assert(queue_tail < sizeof(queue) / sizeof(queue[0]));
	queue[queue_tail++] = (struct fplinux_input_event){
		.type = type,
		.device_id = id,
		.source = source,
		.code = code,
		.pressed = pressed,
	};
}

static void key(uint64_t id, unsigned int code, bool pressed)
{
	event(FPLINUX_INPUT_EVENT_KEY, id, FPLINUX_INPUT_SOURCE_KEYBOARD, code,
	      pressed);
}

static void expect_key(unsigned int code, bool pressed, const char *text)
{
	struct test_mp_tuple *tuple = call("MP_QSTR_read");

	assert(tuple);
	assert(((uintptr_t)tuple->values[1] - 1U) / 2U == code);
	assert(((uintptr_t)tuple->values[2] - 1U) / 2U ==
	       (unsigned int)pressed);
	assert(!strcmp(tuple->values[3], text));
}

mp_obj_t mp_obj_new_int_from_uint(unsigned int value)
{
	return MP_OBJ_NEW_SMALL_INT(value);
}

mp_obj_t mp_obj_new_bool(bool value)
{
	return MP_OBJ_NEW_SMALL_INT(value);
}

mp_obj_t mp_obj_new_str(const char *text, size_t bytes)
{
	static char result[64];

	assert(bytes < sizeof(result));
	memcpy(result, text, bytes);
	result[bytes] = '\0';
	return result;
}

mp_obj_t mp_obj_new_tuple(size_t count, const mp_obj_t *values)
{
	static struct test_mp_tuple tuple;

	assert(count == 4);
	memcpy(tuple.values, values, sizeof(tuple.values));
	return &tuple;
}

_Noreturn void mp_raise_OSError(int error)
{
	(void)error;
	abort();
}

_Noreturn void mp_raise_msg_varg(const void *type, const char *format, ...)
{
	va_list arguments;

	(void)type;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	abort();
}

bool fplinux_input_session_open(struct fplinux_input_session *session,
				unsigned int sources, char *error,
				size_t error_size)
{
	(void)session;
	(void)sources;
	(void)error;
	(void)error_size;
	suspended = false;
	return true;
}

bool fplinux_input_session_next(struct fplinux_input_session *session,
				struct fplinux_input_event *next)
{
	(void)session;
	if (queue_head == queue_tail) {
		queue_head = 0;
		queue_tail = 0;
		return false;
	}
	*next = queue[queue_head++];
	return true;
}

void fplinux_input_session_close(struct fplinux_input_session *session)
{
	(void)session;
}

void fplinux_input_session_suspend(struct fplinux_input_session *session)
{
	(void)session;
	suspended = true;
}

bool fplinux_input_session_resume(struct fplinux_input_session *session,
				  char *error, size_t error_size)
{
	(void)session;
	(void)error;
	(void)error_size;
	suspended = false;
	return true;
}

bool fplinux_keyboard_text_find_layouts(
	const char *root, struct fplinux_keyboard_text_layouts *layouts,
	char *error, size_t size)
{
	(void)root;
	(void)error;
	(void)size;
	memset(layouts, 0, sizeof(*layouts));
	return true;
}

bool fplinux_keyboard_text_keymap(
	const struct fplinux_keyboard_text_layouts *layouts, char *keymap,
	size_t size)
{
	(void)layouts;
	snprintf(keymap, size, "test keymap");
	return true;
}

bool fplinux_keyboard_text_is_printable(const char *text)
{
	return *text != '\0';
}

struct xkb_context *xkb_context_new(unsigned int flags)
{
	(void)flags;
	return &text_context;
}

void xkb_context_unref(struct xkb_context *context)
{
	(void)context;
}

int xkb_context_include_path_append(struct xkb_context *context,
				    const char *path)
{
	(void)context;
	(void)path;
	return 1;
}

struct xkb_keymap *xkb_keymap_new_from_string(struct xkb_context *context,
					      const char *text,
					      unsigned int format,
					      unsigned int flags)
{
	(void)context;
	(void)text;
	(void)format;
	(void)flags;
	return &text_keymap;
}

void xkb_keymap_unref(struct xkb_keymap *keymap)
{
	(void)keymap;
}

struct xkb_state *xkb_state_new(struct xkb_keymap *keymap)
{
	(void)keymap;
	memset(&text_state, 0, sizeof(text_state));
	return &text_state;
}

void xkb_state_unref(struct xkb_state *state)
{
	(void)state;
}

int xkb_state_key_get_utf8(struct xkb_state *state, xkb_keycode_t code,
			   char *text, size_t size)
{
	/* A is the only text key used; the double has one depressed Shift bit. */
	return snprintf(text, size, "%s",
			code == 38 ? (state->shift ? "A" : "a") : "");
}

unsigned int xkb_state_update_key(struct xkb_state *state, xkb_keycode_t code,
				  enum xkb_key_direction direction)
{
	if (code == 50)
		state->shift = direction == XKB_KEY_DOWN;
	return 0;
}

int main(void)
{
	call("MP_QSTR_open");
	event(FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1,
	      FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false);
	event(FPLINUX_INPUT_EVENT_DEVICE_ADDED, 2,
	      FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false);
	key(1, 42, true);
	key(2, 42, true);
	expect_key(42, true, "");
	expect_key(42, true, "");
	/* Unplug releases keyboard 1; keyboard 2 still owns the modifier. */
	key(1, 42, false);
	event(FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 1,
	      FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false);
	key(2, 30, true);
	expect_key(30, true, "A");
	key(2, 30, false);
	expect_key(30, false, "");
	key(2, 42, false);
	expect_key(42, false, "");
	key(2, 30, true);
	expect_key(30, true, "a");
	key(2, 30, false);
	expect_key(30, false, "");
	/* A queued UI event must not escape after the active application yields. */
	key(2, 42, true);
	assert(call("MP_QSTR_pending") == MP_OBJ_NEW_SMALL_INT(1));
	key(2, 42, false);
	event(FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 2,
	      FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false);
	assert(fplinux_keypad_set_active(false));
	assert(suspended && !text_state.shift);
	assert(call("MP_QSTR_read") == mp_const_none);
	assert(fplinux_keypad_set_active(true));
	assert(!suspended);
	event(FPLINUX_INPUT_EVENT_DEVICE_ADDED, 3,
	      FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false);
	key(3, 30, true);
	expect_key(30, true, "a");
	call("MP_QSTR_close");
	return 0;
}
