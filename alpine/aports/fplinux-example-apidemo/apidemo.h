/* SPDX-License-Identifier: 0BSD */
/*
 * API Demos: a catalogue of small screens, each showing one part of the
 * FPLinux UI runtime (libfplinux-ui.so.1), live. The permanent catalogue,
 * search and examples share the runtime's screen stack.
 */
#ifndef FPLINUX_APIDEMO_H
#define FPLINUX_APIDEMO_H

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-locale.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-text.h>
#include <libintl.h>
#include <stdbool.h>
#include <stddef.h>

#define APIDEMO_APP_ID "org.fplinux.example.apidemo"
/* The package's own images, read by the runtime when a demo shows them. */
#ifndef APIDEMO_IMAGE_DIR
#define APIDEMO_IMAGE_DIR "/usr/share/fplinux-example-apidemo/"
#endif
/* A colour picture, 240 x 160 pixels and opaque, so it reads on both themes. */
#define APIDEMO_PHOTO APIDEMO_IMAGE_DIR "photo.png"
/* An alpha-only shape, 48 x 48 pixels: drawn in the colour of its context. */
#define APIDEMO_SHAPE APIDEMO_IMAGE_DIR "star-symbolic.png"

#define _(text) gettext(text)
/* Marks text for translation where it is stored; _() translates on use. */
#define N_(text) text
#define APIDEMO_COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* What every demo may use. The runtime owns metrics and theme. */
struct apidemo {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *catalogue;
	/* An existing confirmation or saving error still needs its answer. */
	struct fplinux_ui_screen *decision;
	/* The application's preferences, or NULL when they cannot be read. */
	struct fplinux_ui_prefs *prefs;
	const struct fplinux_ui_metrics *metrics;
	const struct fplinux_ui_theme *theme;
	/* false while the display is off or the keypad is locked. */
	bool visible;
};

/* Saves memory values; background failures update status without an alert. */
struct fplinux_storage_result apidemo_save_prefs(struct apidemo *app,
						 bool interactive);

struct apidemo_entry;

/* Builds the demo's screen and pushes it; reports a failure itself. */
typedef void (*apidemo_open_cb)(struct apidemo *app,
				const struct apidemo_entry *entry);

struct apidemo_entry {
	/* N_() text: the catalogue row and the title of the demo screen. */
	const char *title;
	/* C names for search and the focused example's Help command. */
	const char *detail;
	apidemo_open_cb open;
	/* For entries that share one open function. */
	int variant;
	/* N_() text describing the result in the catalogue's second line. */
	const char *description;
};

struct apidemo_category {
	const struct apidemo_entry *entries;
	size_t count;
	/* Optional hooks, NULL for none. The application was hidden or shown:
	 * pause or resume timers. */
	void (*visibility)(struct apidemo *app, bool visible);
	/* CLOSE arrived: save what the user would not want to lose. */
	void (*close)(struct apidemo *app);
	/* Before OPEN removes section screens or fplinux_ui_close(): delete
	 * timers, stop watching descriptors, release input devices and forget
	 * owned screen pointers. Screens may still be on the stack. */
	void (*cleanup)(struct apidemo *app);
	/* FPLINUX_UI_EVENT_RESIZE: the surface has a new size, the screen
	 * stack is laid out again; lay out raw objects and canvases. */
	void (*resize)(struct apidemo *app);
};

/* Example groups; each source file owns its resource hooks. */
extern const struct apidemo_category apidemo_lists;
extern const struct apidemo_category apidemo_forms;
extern const struct apidemo_category apidemo_alerts;
extern const struct apidemo_category apidemo_text;
extern const struct apidemo_category apidemo_commands;
extern const struct apidemo_category apidemo_softkeys;
extern const struct apidemo_category apidemo_canvas;
extern const struct apidemo_category apidemo_frame;
extern const struct apidemo_category apidemo_look;
extern const struct apidemo_category apidemo_locale;
extern const struct apidemo_category apidemo_lifecycle;
extern const struct apidemo_category apidemo_vibration;
extern const struct apidemo_category apidemo_sound;

/*
 * The shared Back command: a BACK command on the right soft key with the
 * runtime's own label. Demos keep their own command ids below it.
 */
#define APIDEMO_COMMAND_BACK 1000
extern const struct fplinux_ui_command apidemo_back_command;

/* Adds apidemo_back_command; false with errno as add_command(). */
bool apidemo_add_back(struct fplinux_ui_screen *screen);
/* A command listener that returns on APIDEMO_COMMAND_BACK and ignores the
 * rest, for screens with nothing else to do. */
void apidemo_back_listener(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int command_id,
			   void *context);
/*
 * Pushes screen, the result of the constructor named by call, or reports
 * the constructor's errno when screen is NULL. Returns whether it pushed.
 */
bool apidemo_push(struct apidemo *app, struct fplinux_ui_screen *screen,
		  const char *call);
/* An information alert with Close: results, values, explanations. */
void apidemo_show(struct apidemo *app, const char *title, const char *text);
void apidemo_showf(struct apidemo *app, const char *title, const char *format,
		   ...) __attribute__((format(printf, 3, 4)));
/* Logs "call: strerror(errno)" and shows it in an error alert. */
struct fplinux_ui_screen *apidemo_error(struct apidemo *app, const char *call);
/* The symbolic name of an errno value, such as "EINVAL". */
const char *apidemo_errno_name(int error);
/* A system glyph at the row size, or NULL for an unknown name. */
const void *apidemo_glyph(struct apidemo *app, const char *name);
/*
 * Gives a vertically scrolling LVGL object the system's scroll indicator:
 * a thumb in the focus colour at the right edge, no track. Scroll it with
 * fplinux_ui_scroll_key().
 */
void apidemo_style_scroll(struct apidemo *app, lv_obj_t *object);
/* A delayed manager error updates the request screen or opens an alert. */
void apidemo_vibration_error(struct apidemo *app, const char *reason);
/* Keep a saving failure visible until its explicit Close command. */
void apidemo_protect_error(struct apidemo *app,
			   struct fplinux_ui_screen *alert);

#endif
