/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

static xkb_keycode_t keycode(struct xkb_keymap *keymap, const char *name)
{
	xkb_keycode_t code = xkb_keymap_key_by_name(keymap, name);

	assert(code != XKB_KEYCODE_INVALID);
	return code;
}

static void assert_text(struct xkb_state *state, xkb_keycode_t code,
			const char *expected)
{
	char text[16];

	assert(xkb_state_key_get_utf8(state, code, text, sizeof(text)) ==
	       (int)strlen(expected));
	assert(!strcmp(text, expected));
}

static void test_layouts(struct xkb_keymap *keymap)
{
	struct xkb_state *state = xkb_state_new(keymap);
	xkb_keycode_t letter = keycode(keymap, "AC01");
	xkb_keycode_t alt = keycode(keymap, "LALT");
	xkb_keycode_t shift = keycode(keymap, "LFSH");
	xkb_keycode_t function = keycode(keymap, "FK13");

	assert(state);
	assert(xkb_keymap_num_layouts(keymap) == 2);
	assert(xkb_state_key_get_layout(state, letter) == 0);
	assert_text(state, letter, "a");
	assert(xkb_state_key_get_one_sym(state, function) == XKB_KEY_F13);
	xkb_state_update_key(state, alt, XKB_KEY_DOWN);
	xkb_state_update_key(state, shift, XKB_KEY_DOWN);
	xkb_state_update_key(state, shift, XKB_KEY_UP);
	xkb_state_update_key(state, alt, XKB_KEY_UP);
	assert(xkb_state_key_get_layout(state, letter) == 1);
	assert_text(state, letter, "ф");
	assert(xkb_state_key_get_one_sym(state, function) == XKB_KEY_F13);
	xkb_state_unref(state);
}

int main(int argc, char **argv)
{
	const struct xkb_rule_names names = {
		.rules = "fplinux",
		.model = "pc105",
		.layout = "us,ru",
		.variant = "",
		.options = "group:alt_shift_toggle",
	};
	struct xkb_context *context;
	struct xkb_keymap *keymap;

	assert(argc == 2);
	context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES |
				  XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
	assert(context);
	assert(xkb_context_include_path_append(context, argv[1]));
	keymap = xkb_keymap_new_from_names(context, &names,
					   XKB_KEYMAP_COMPILE_NO_FLAGS);
	assert(keymap);
	test_layouts(keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	return 0;
}
