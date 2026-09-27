/* SPDX-License-Identifier: GPL-2.0-only */
/* Single-layout XKB double to observe the adapter's physical-key accounting. */
#ifndef FPLINUX_TEST_XKBCOMMON_H
#define FPLINUX_TEST_XKBCOMMON_H

#include <stddef.h>
#include <stdint.h>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;
typedef uint32_t xkb_keycode_t;
enum xkb_key_direction {
	XKB_KEY_UP,
	XKB_KEY_DOWN,
};

#define XKB_CONTEXT_NO_DEFAULT_INCLUDES 1U
#define XKB_CONTEXT_NO_ENVIRONMENT_NAMES 2U
#define XKB_KEYMAP_FORMAT_TEXT_V1 1U
#define XKB_KEYMAP_COMPILE_NO_FLAGS 0U

struct xkb_context *xkb_context_new(unsigned int flags);
void xkb_context_unref(struct xkb_context *context);
int xkb_context_include_path_append(struct xkb_context *context,
				    const char *path);
struct xkb_keymap *xkb_keymap_new_from_string(struct xkb_context *context,
					      const char *text,
					      unsigned int format,
					      unsigned int flags);
void xkb_keymap_unref(struct xkb_keymap *keymap);
struct xkb_state *xkb_state_new(struct xkb_keymap *keymap);
void xkb_state_unref(struct xkb_state *state);
int xkb_state_key_get_utf8(struct xkb_state *state, xkb_keycode_t code,
			   char *text, size_t size);
unsigned int xkb_state_update_key(struct xkb_state *state, xkb_keycode_t code,
				  enum xkb_key_direction direction);

#endif
