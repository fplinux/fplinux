/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_H
#define FPLINUX_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lvgl/lvgl.h>

#include "fplinux-storage.h"

struct fplinux_ui;
/* The session protocol of fplinux-ui-session.h, for system components. */
struct fplinux_ui_session_message;

enum fplinux_ui_key_source {
	FPLINUX_UI_KEY_PHONE,
	FPLINUX_UI_KEY_KEYBOARD,
};

/*
 * code is a Linux KEY_* value for both sources; text is keyboard UTF-8.
 * The layout is fixed for libfplinux-ui.so.1: applications may build a key
 * themselves and pass it to the functions that take one.
 */
struct fplinux_ui_key {
	enum fplinux_ui_key_source source;
	unsigned int code;
	uint32_t keysym;
	uint32_t unicode;
	uint32_t ascii;
	bool pressed;
	bool shift;
	bool control;
	bool alt;
	char text[16];
	/* Runtime CLOCK_MONOTONIC milliseconds, including synthetic input. */
	uint64_t time_ms;
};

enum fplinux_ui_event_type {
	FPLINUX_UI_EVENT_KEY,
	FPLINUX_UI_EVENT_SESSION,
	FPLINUX_UI_EVENT_CLOSE,
	FPLINUX_UI_EVENT_VISIBILITY,
	/*
	 * The surface changed size, after fplinux_ui_set_fullscreen():
	 * fplinux_ui_width(), fplinux_ui_height() and fplinux_ui_is_fullscreen()
	 * give the new state and the screen stack is laid out again. Lay out
	 * the raw screen and Canvas drawings again; the frame drawn next is the
	 * first one shown at the new size.
	 */
	FPLINUX_UI_EVENT_RESIZE,
	/* Query fplinux_ui_lifecycle(); event layout is unchanged. */
	FPLINUX_UI_EVENT_LIFECYCLE,
	/* Held input was cancelled; key.source identifies the affected seat type. */
	FPLINUX_UI_EVENT_INPUT_CANCEL,
};

enum fplinux_ui_lifecycle {
	FPLINUX_UI_FOREGROUND,
	FPLINUX_UI_BACKGROUND,
	FPLINUX_UI_CLOSING,
};

/*
 * The runtime fills an event and passes it to the callback, valid only during
 * the call. Later versions of libfplinux-ui.so.1 may append fields and event
 * types: ignore unknown types and never allocate an event yourself.
 */
struct fplinux_ui_event {
	enum fplinux_ui_event_type type;
	/* KEY, or the source of INPUT_CANCEL before synthetic held-key releases. */
	struct fplinux_ui_key key;
	/* VISIBILITY: false when the display is off or locked. */
	bool visible;
	/*
	 * SESSION, otherwise NULL: a manager reply or system component message.
	 * Its layout follows the session protocol, not this ABI. An attached
	 * frame descriptor is borrowed for this callback; the runtime closes it.
	 */
	const struct fplinux_ui_session_message *session;
};

typedef void (*fplinux_ui_event_cb)(struct fplinux_ui *ui,
				    const struct fplinux_ui_event *event,
				    void *context);
typedef void (*fplinux_ui_fd_cb)(struct fplinux_ui *ui, int fd, short events,
				 void *context);

enum fplinux_ui_close_response {
	FPLINUX_UI_CLOSE_READY,
	FPLINUX_UI_CLOSE_CANCEL,
	FPLINUX_UI_CLOSE_WAIT,
};

typedef enum fplinux_ui_close_response (*fplinux_ui_close_cb)(
	struct fplinux_ui *ui, void *context);

enum fplinux_ui_theme_kind {
	FPLINUX_UI_THEME_LIGHT,
	FPLINUX_UI_THEME_DARK,
};

/*
 * The system theme: semantic colours, exact in RGB565, and the profile's
 * fonts. Owned by the runtime and valid until fplinux_ui_close(); read it
 * through fplinux_ui_theme(). Later versions may append fields: never copy
 * the structure or depend on its size. Use the roles, not the values.
 */
struct fplinux_ui_theme {
	lv_color_t background; /* page */
	lv_color_t surface; /* header band, form and field frames */
	lv_color_t foreground; /* text and glyphs on background, surface, popup */
	lv_color_t muted; /* secondary text: details, labels, hints */
	lv_color_t accent; /* selected row or cell, pending character */
	lv_color_t accent_text; /* text and glyphs on accent */
	lv_color_t border; /* dividers, unfocused frames, tracks */
	const lv_font_t *font; /* body; size follows the profile */
	const lv_font_t *font_large; /* titles and emphasis, never smaller */
	/* Large clocks and numbers: digits, space and + , - . : E e only. */
	const lv_font_t *font_clock;
	enum fplinux_ui_theme_kind kind;
	lv_color_t popup; /* dialogs, options menu, notices, picker */
	lv_color_t bar; /* status and soft-key bars */
	lv_color_t bar_text; /* text and glyphs on bar */
	lv_color_t disabled; /* unavailable items, placeholders */
	lv_color_t focus; /* focus ring, gauge fill, spinner, scrollbar */
	lv_color_t warning; /* warning dialog frame and title */
	lv_color_t danger; /* error dialog frame and title, battery low */
	lv_color_t tile_ring; /* ring around a colour tile on accent */
	lv_color_t brand; /* decorative brand accent; never text */
	/* Soft-key legends: the body font, or a narrower face. */
	const lv_font_t *font_softkey;
	/* Static backdrop behind a modal; dialogs themselves stay opaque. */
	lv_color_t scrim;
	lv_opa_t scrim_opacity;
};

/*
 * Screen profiles. The runtime uses the largest pixel area that fits in the
 * display, with earlier profiles winning ties; layouts use the real surface
 * size. Later versions may add profiles: give a switch a default case and
 * do not compare the values.
 */
enum fplinux_ui_profile {
	FPLINUX_UI_PROFILE_128X128,
	FPLINUX_UI_PROFILE_128X160,
	FPLINUX_UI_PROFILE_176X220,
	FPLINUX_UI_PROFILE_240X320,
	FPLINUX_UI_PROFILE_320X240,
};

/*
 * Layout metrics in pixels. Owned by the runtime and valid until
 * fplinux_ui_close(); later versions may append fields.
 */
struct fplinux_ui_metrics {
	enum fplinux_ui_profile profile;
	int screen_width; /* the whole display */
	int screen_height;
	int status_height; /* launcher bars around an integrated surface */
	int softkey_height;
	int header_height; /* fplinux_ui_header() and titled screens */
	int margin; /* left inset of text and content */
	int gutter; /* right inset of scrolling content */
	int spacing; /* gap between items */
	int gap_small; /* gap inside an item */
	int row_height; /* list, menu and catalogue rows */
	int row_height_detail; /* rows with a second line */
	int row_icon_size; /* square icon boxes */
	int status_icon_size;
	int app_icon_size;
	int grid_columns; /* the launcher grid, for app grids too */
	int focus_width; /* focus ring and frame width */
	int frame_padding; /* inside a framed control: gauge, clock face */
};

/* Open waits for manager CONFIG before creating any Wayland surface. */
struct fplinux_ui *fplinux_ui_open(const char *app_id,
				   fplinux_ui_event_cb callback, void *context);
void fplinux_ui_close(struct fplinux_ui *ui);
/* Run returns 0 after normal CLOSE/quit, -1 on a connection/rendering error. */
int fplinux_ui_run(struct fplinux_ui *ui);
void fplinux_ui_quit(struct fplinux_ui *ui);
/*
 * Ordinary user close calls this handler before final CLOSE. READY permits
 * closing, CANCEL keeps the application, and WAIT keeps its decision UI
 * interactive until close_reply(). Without a handler the runtime is ready.
 * Forced CLOSE, signals and connection failure still deliver EVENT_CLOSE
 * and exit without waiting. The handler does not replace that final event.
 */
void fplinux_ui_set_close_handler(struct fplinux_ui *ui,
				  fplinux_ui_close_cb callback, void *context);
/* Answers the current WAIT; false with EINVAL when none is pending. */
bool fplinux_ui_close_reply(struct fplinux_ui *ui,
			    enum fplinux_ui_close_response response);
/* Translated errno cause; the application presents publication state separately. */
/* Empty for DURABLE, including an unchanged save. */
const char *fplinux_ui_storage_error(struct fplinux_storage_result result);
/* One optional descriptor; fd ownership stays with the caller. */
void fplinux_ui_watch_fd(struct fplinux_ui *ui, int fd, short events,
			 fplinux_ui_fd_cb callback, void *context);

lv_obj_t *fplinux_ui_screen(struct fplinux_ui *ui);
int fplinux_ui_width(const struct fplinux_ui *ui);
int fplinux_ui_height(const struct fplinux_ui *ui);
const struct fplinux_ui_theme *fplinux_ui_theme(const struct fplinux_ui *ui);
const struct fplinux_ui_metrics *
fplinux_ui_metrics(const struct fplinux_ui *ui);

/*
 * Apply the system scrollbar's colour, width and inset to a raw LVGL
 * object. Scroll direction, visibility mode and vertical padding belong
 * to its viewport. The object and UI remain owned by the caller.
 */
void fplinux_ui_style_scrollbar(struct fplinux_ui *ui, lv_obj_t *object);

/*
 * A system glyph by name, at the profile's
 * row_icon_size or status_icon_size, as an alpha-only LVGL image source owned
 * by the runtime until fplinux_ui_close(). Rows, menus and form images draw
 * it in their text colour; an lv_image draws it in its image_recolor colour.
 * NULL with errno ENOENT for an unknown name.
 */
enum fplinux_ui_glyph_size {
	FPLINUX_UI_GLYPH_ROW,
	FPLINUX_UI_GLYPH_STATUS,
};
const void *fplinux_ui_glyph(struct fplinux_ui *ui, const char *name,
			     enum fplinux_ui_glyph_size size);

/*
 * Decodes an image source once (a PNG path or an lv_image_dsc_t, as
 * fplinux_ui_list_set_icon() takes it), scaled down with an area filter to
 * fit width x height with its proportions kept, never up. A file named
 * "*-symbolic.png" becomes alpha-only. The caller owns the buffer: show it
 * with lv_image_set_src() and free it with lv_draw_buf_destroy() after the
 * image object is deleted. NULL with errno as fplinux_ui_list_set_icon().
 */
lv_draw_buf_t *fplinux_ui_image_load(const void *src, int width, int height);
uint64_t fplinux_ui_now_ms(void);
/* A raw private session message, also used for system-app loading phases. */
bool fplinux_ui_send(struct fplinux_ui *ui,
		     const struct fplinux_ui_session_message *message);
bool fplinux_ui_set_title(struct fplinux_ui *ui, const char *title);
/* Legends for the left soft key, OK and the right soft key; "" for none. */
bool fplinux_ui_set_softkeys(struct fplinux_ui *ui, const char *left,
			     const char *centre, const char *right);
/*
 * A legend bar for an application-drawn fullscreen screen or Canvas.
 * It fills parent's width, aligned at the bottom, softkey_height high.
 * NULL with EINVAL for a NULL parent, ENOMEM on allocation failure.
 * Parent owns it; lv_obj_delete() may delete it sooner. Leave its three
 * children to the helper. It neither sends session legends nor handles keys.
 */
lv_obj_t *fplinux_ui_softkey_bar_create(struct fplinux_ui *ui,
					lv_obj_t *parent);
/*
 * Updates a bar created above, fitting UTF-8 legends in its current width.
 * NULL or "" hides a legend. Call again after resizing its parent or bar.
 * Strings must be shorter than 128 bytes and contain no control characters:
 * false with ENAMETOOLONG/EINVAL, leaving all legends unchanged.
 */
bool fplinux_ui_softkey_bar_set(struct fplinux_ui *ui, lv_obj_t *bar,
				const char *left, const char *centre,
				const char *right);
/* Standard title band at the top of the screen; content starts below it. */
lv_obj_t *fplinux_ui_header(struct fplinux_ui *ui, const char *title);
/* Request ordinary close and the catalogue (Back on the first screen). */
bool fplinux_ui_back(struct fplinux_ui *ui);
/* Background the application for the home view (system Home). */
bool fplinux_ui_home(struct fplinux_ui *ui);
/* Keep the display on while this application is visible; false releases. */
bool fplinux_ui_keep_screen_on(struct fplinux_ui *ui, bool enabled);
bool fplinux_ui_is_visible(const struct fplinux_ui *ui);
/* Home/reopen/close state; lock, display-off and overlays do not change it. */
enum fplinux_ui_lifecycle fplinux_ui_lifecycle(const struct fplinux_ui *ui);
/*
 * Switches the application between the two display modes while it runs,
 * whichever its desktop entry declares: true for the whole display without
 * the system bars, false for the surface between the launcher's status and
 * soft-key bars. The manager resizes the surface; FPLINUX_UI_EVENT_RESIZE
 * then reports the new size. Asking for the mode already asked for sends
 * nothing. Keys and the screen stack work alike in both modes. Each start
 * begins in the declared mode. Fails with EINVAL in the launcher.
 */
bool fplinux_ui_set_fullscreen(struct fplinux_ui *ui, bool fullscreen);
/* The surface covers the whole display, without the system bars. */
bool fplinux_ui_is_fullscreen(const struct fplinux_ui *ui);
/*
 * The system language code from the manager, such as "en" or "ru": the name
 * of its file in /usr/share/fplinux/languages. Messages follow it.
 */
const char *fplinux_ui_locale(const struct fplinux_ui *ui);
/* Preferred language for a new text field; existing fields keep their mode. */
const char *fplinux_ui_input_locale(const struct fplinux_ui *ui);
/* Returns an owned per-application XDG state directory, creating it mode 0700. */
bool fplinux_ui_state_dir(const char *app_id, char *path, size_t capacity);
/* Phone KEY_F13/F14 only. External function keys never become phone softkeys. */
bool fplinux_ui_is_softkey(const struct fplinux_ui_key *key, bool left);

#endif
