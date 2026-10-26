/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "fplinux-ui.h"
#include "fplinux-audio-client-internal.h"
#include "fplinux-audio-protocol.h"
#include "fplinux-language.h"
#include "fplinux-ui-audio.h"
#include "fplinux-ui-fonts.h"
#include "fplinux-ui-frame-private.h"
#include "fplinux-ui-internal.h"
#include "fplinux-ui-menu.h"
#include "fplinux-ui-prefs.h"
#include "fplinux-ui-screen.h"
#include "fplinux-ui-profile.h"
#include "fplinux-ui-screen-model.h"
#include "fplinux-ui-session.h"
#include "fplinux-ui-softkey-layout.h"
#include "fplinux-ui-vibration.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#define FPLINUX_UI_BUFFER_COUNT 2
/* LVGL draws in partial mode into a buffer of this many surface rows. */
#define FPLINUX_UI_DRAW_ROWS 32
#define FPLINUX_UI_CONFIG_TIMEOUT_MS 5000
/* The largest display side that CONFIG may report. */
#define FPLINUX_UI_SCREEN_MAX 4096
/* Catalogues: <directory>/<language>/LC_MESSAGES/<domain>.mo */
#define FPLINUX_UI_LOCALE_DIRECTORY "/usr/share/locale"
/* The longest system glyph name, as in "screen-timeout". */
#define FPLINUX_UI_GLYPH_NAME_BYTES 64
/* A row or form marquee: 400 ms before the first pass, 30 px/s, 1 s pauses. */
#define MARQUEE_DELAY_MS 400
#define MARQUEE_PAUSE_MS 1000
#define MARQUEE_SPEED_PX_S 30
#define MARQUEE_MIN_MS 300
#define MARQUEE_MAX_MS 10000

_Static_assert(FPLINUX_UI_AUDIO_RATE_MIN == FPLINUX_AUDIO_RATE_MIN &&
		       FPLINUX_UI_AUDIO_RATE_MAX == FPLINUX_AUDIO_RATE_MAX &&
		       FPLINUX_UI_AUDIO_STREAMS_MAX ==
			       FPLINUX_AUDIO_CLIENT_STREAMS,
	       "the public audio formats match the stream transport");

/* The theme's font slots, as the profile table names them. */
enum font_slot {
	FONT_BODY,
	FONT_TITLE,
	FONT_CLOCK,
	FONT_LEGEND,
	FONT_SLOTS,
};

/* A system glyph decoded once, alpha only, kept until close. */
struct glyph {
	struct glyph *next;
	int size;
	lv_draw_buf_t *image;
	char name[FPLINUX_UI_GLYPH_NAME_BYTES];
};

/*
 * A title's one marquee pass, else the focused
 * element's labels in turn. labels[turn] is the label that scrolls or
 * waits for its pass; armed while it is in LV_LABEL_LONG_MODE_SCROLL.
 */
struct marquee {
	lv_obj_t *title;
	lv_obj_t *labels[2];
	unsigned int count;
	unsigned int turn;
	bool armed;
	/* labels[turn] has been measured since it was armed. */
	bool checked;
	/* Bits of the labels found to fit since the element took focus. */
	unsigned int fitting;
	/* A pass of the focused element has completed since then. */
	bool passed;
};

struct frame_buffer {
	struct wl_buffer *buffer;
	void *pixels;
	size_t bytes;
	bool busy;
};

enum repeat_target {
	REPEAT_NONE,
	REPEAT_MENU,
	REPEAT_SCREEN,
	REPEAT_RAW_TEXT,
};

struct repeat_owner {
	enum repeat_target target;
	union {
		struct fplinux_ui_menu_view *menu;
		struct fplinux_ui_screen *screen;
		struct fplinux_ui_text *field;
	};
};

struct text_key_claim {
	struct text_key_claim *previous;
	const struct fplinux_ui_key *key;
	struct fplinux_ui_text *field;
};

struct input_seat {
	struct input_seat *next;
	struct fplinux_ui *ui;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct xkb_keymap *keymap;
	struct xkb_state *key_state;
	uint32_t global_name;
	enum fplinux_ui_key_source source;
	bool focused;
	/* Only presses delivered to this surface can acquire a release. */
	unsigned char pressed[KEY_CNT / 8];
	lv_timer_t *repeat_timer;
	struct repeat_owner repeat_owner;
	uint64_t repeat_generation;
	unsigned int repeat_code;
	int32_t repeat_rate;
	int32_t repeat_delay;
};

struct frame_ack {
	struct fplinux_ui *ui;
	enum fplinux_ui_session_message_type type;
	uint32_t serial;
	struct wl_callback *sync;
};

enum frame_ack_slot {
	FRAME_ACK_ACTIVE,
	FRAME_ACK_ACTIVATION,
	FRAME_ACK_APPEARANCE,
	FRAME_ACK_CLOSE,
	FRAME_ACK_COUNT,
};

struct fplinux_ui {
	int session_fd;
	int width;
	int height;
	int watch_fd;
	short watch_events;
	fplinux_ui_fd_cb watch_callback;
	void *watch_context;
	fplinux_ui_event_cb callback;
	void *context;
	fplinux_ui_close_cb close_callback;
	void *close_context;
	uint32_t close_serial;
	enum fplinux_ui_close_response close_response;
	struct wl_display *wayland;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct input_seat *seats;
	/* A dispatch cannot arm an owner that its callback covered or deleted. */
	uint64_t repeat_generation;
	/* A raw field may claim only this fallback application's exact key. */
	struct text_key_claim *text_claim;
	struct wl_surface *surface;
	struct xdg_wm_base *wm;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct xkb_context *xkb_context;
	/*
	 * The SHM buffers of the current size, buffer_sets[buffer_set]. After
	 * a resize the other set keeps the previous size's buffers, the
	 * surface's content, until the first buffer of the new size is
	 * committed (old_buffers).
	 */
	struct frame_buffer buffer_sets[2][FPLINUX_UI_BUFFER_COUNT];
	unsigned int buffer_set;
	bool old_buffers;
	/* CONFIG.mode, and the mode that MODE asked for last. */
	enum fplinux_ui_session_mode config_mode;
	enum fplinux_ui_session_mode requested_mode;
	/* Whether the latest Wayland configure covers the whole display. */
	bool fullscreen;
	/*
	 * The last xdg_toplevel.configure: its size, 0 for the current one,
	 * and its state, FULLSCREEN or MAXIMIZED (has_state) or neither.
	 */
	int configure_width;
	int configure_height;
	bool configure_fullscreen;
	bool configure_has_state;
	/*
	 * The configure of the new size waits for the commit of the first
	 * buffer of that size: Weston rejects a maximized surface that commits
	 * another size than the configure it acknowledged.
	 */
	bool ack_pending;
	uint32_t ack_serial;
	lv_display_t *display;
	uint16_t *canvas;
	void *draw_buffer;
	const lv_font_t *fonts[FONT_SLOTS];
	const struct fplinux_ui_profile_spec *spec;
	struct fplinux_ui_theme theme;
	struct fplinux_ui_metrics metrics;
	struct marquee marquee;
	struct glyph *glyphs;
	char locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	char input_locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	/* The legends the application set last; an open menu shows its own. */
	char softkeys[3][FPLINUX_UI_SESSION_TEXT_BYTES];
	/* The legends sent last, which the modal legend bar shows. */
	char legends[3][FPLINUX_UI_SESSION_TEXT_BYTES];
	/* enum fplinux_ui_modal bits of the modal elements that are up. */
	unsigned int modals;
	/* The soft-key bar of a fullscreen application's modals, or NULL. */
	lv_obj_t *legend_bar;
	struct fplinux_ui_menu_view *menu;
	fplinux_ui_menu_cb menu_callback;
	void *menu_context;
	/* Presses that went to the menu, by source and code; so do releases. */
	unsigned char menu_keys[2][KEY_CNT / 8];
	/* The high-level screen stack, or NULL. */
	struct fplinux_ui_screens *screens;
	bool has_rgb565;
	bool configured;
	bool dirty;
	bool ready;
	bool frame_submitted;
	/* Independent transitions may acknowledge the same complete committed view. */
	struct frame_ack frame_acks[FRAME_ACK_COUNT];
	/* Keep the submitted view until the manager completes an EXIT. */
	bool exit_pending;
	bool visible;
	bool audio;
	bool vibration;
	struct fplinux_audio_client *audio_client;
	enum fplinux_ui_lifecycle lifecycle;
	bool quit;
	bool failed;
};

static volatile sig_atomic_t stop_signal;

static void queue_frame_ack(struct fplinux_ui *ui,
			    enum fplinux_ui_session_message_type type,
			    uint32_t serial);

static void marquee_set_visible(struct fplinux_ui *ui, bool visible);
static void marquee_restart(struct fplinux_ui *ui);
static void resize(struct fplinux_ui *ui, int width, int height,
		   bool fullscreen);
static void show_legend_bar(struct fplinux_ui *ui);
static void set_theme_kind(struct fplinux_ui *ui, const char *value);
static void layout_done(lv_event_t *event);
static void release_keys(struct input_seat *seat);

static void stop_repeat(struct input_seat *seat)
{
	seat->repeat_owner = (struct repeat_owner){ 0 };
	if (seat->repeat_timer)
		lv_timer_pause(seat->repeat_timer);
}

void fplinux_ui_cancel_key_repeat(struct fplinux_ui *ui,
				  const struct fplinux_ui_screen *screen)
{
	if (!ui)
		return;
	++ui->repeat_generation;
	for (struct text_key_claim *claim = ui->text_claim; claim;
	     claim = claim->previous)
		claim->field = NULL;
	for (struct input_seat *seat = ui->seats; seat; seat = seat->next)
		if (!screen || (seat->repeat_owner.target == REPEAT_SCREEN &&
				seat->repeat_owner.screen == screen))
			stop_repeat(seat);
}

void fplinux_ui_claim_text_key(struct fplinux_ui *ui,
			       struct fplinux_ui_text *field,
			       const struct fplinux_ui_key *key)
{
	if (ui && ui->text_claim && ui->text_claim->key == key && key->pressed)
		ui->text_claim->field = field;
}

void fplinux_ui_cancel_text_key_repeat(struct fplinux_ui *ui,
				       const struct fplinux_ui_text *field)
{
	bool cancelled = false;

	if (!ui || !field)
		return;
	for (struct text_key_claim *claim = ui->text_claim; claim;
	     claim = claim->previous)
		if (claim->field == field) {
			claim->field = NULL;
			cancelled = true;
		}
	for (struct input_seat *seat = ui->seats; seat; seat = seat->next) {
		if (seat->repeat_owner.target != REPEAT_RAW_TEXT ||
		    seat->repeat_owner.field != field)
			continue;
		stop_repeat(seat);
		cancelled = true;
	}
	if (cancelled)
		++ui->repeat_generation;
}

_Static_assert(FPLINUX_UI_PROFILE_320X240 + 1 == FPLINUX_UI_PROFILE_COUNT,
	       "the profile table follows enum fplinux_ui_profile");
_Static_assert((int)FPLINUX_UI_THEME_DARK == (int)FPLINUX_UI_SESSION_THEME_DARK,
	       "the session theme is the theme kind");

uint64_t fplinux_ui_now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint32_t tick_ms(void)
{
	return (uint32_t)fplinux_ui_now_ms();
}

static void stop_handler(int signal_number)
{
	stop_signal = signal_number;
}

static bool send_softkeys(struct fplinux_ui *ui, const char *left,
			  const char *centre, const char *right)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SOFTKEYS,
	};

	/* A translated legend is catalogue data: cut rather than overflow. */
	snprintf(message.left, sizeof(message.left), "%s", left);
	snprintf(message.centre, sizeof(message.centre), "%s", centre);
	snprintf(message.right, sizeof(message.right), "%s", right);
	strcpy(ui->legends[0], message.left);
	strcpy(ui->legends[1], message.centre);
	strcpy(ui->legends[2], message.right);
	show_legend_bar(ui);
	return fplinux_ui_send(ui, &message);
}

/* The callback runs after the menu closed, so that it may open another. */
static void finish_menu(struct fplinux_ui *ui, int action)
{
	fplinux_ui_menu_cb callback = ui->menu_callback;
	void *context = ui->menu_context;
	struct fplinux_ui_menu_position position;

	fplinux_ui_menu_view_get_position(ui->menu, &position);
	fplinux_ui_screens_menu_position(ui->screens, &position);
	fplinux_ui_menu_close(ui);
	if (callback)
		callback(ui, action, context);
}

/*
 * An open menu takes every press. A release goes where its press went, so
 * the application neither misses the release of a key it saw pressed nor
 * receives the release of the key that closed the menu.
 */
static enum fplinux_ui_key_route
route_menu_key(struct fplinux_ui *ui, const struct fplinux_ui_key *key)
{
	bool keyboard = key->source == FPLINUX_UI_KEY_KEYBOARD;
	unsigned char bit = (unsigned char)(1U << (key->code % 8));
	unsigned char *pressed;
	int action;

	if (key->code >= KEY_CNT)
		return ui->menu ? FPLINUX_UI_KEY_HANDLED :
				  FPLINUX_UI_KEY_UNHANDLED;
	pressed = &ui->menu_keys[keyboard][key->code / 8];
	if (key->pressed) {
		if (!ui->menu)
			return FPLINUX_UI_KEY_UNHANDLED;
		*pressed |= bit;
	} else {
		if (!(*pressed & bit))
			return FPLINUX_UI_KEY_UNHANDLED;
		*pressed &= (unsigned char)~bit;
		if (!ui->menu)
			return FPLINUX_UI_KEY_HANDLED;
	}
	if (fplinux_ui_menu_view_key(ui->menu, key, &action)) {
		finish_menu(ui, action);
		return FPLINUX_UI_KEY_HANDLED;
	}
	return key->pressed && fplinux_ui_is_direction_key(key) ?
		       FPLINUX_UI_KEY_REPEATABLE :
		       FPLINUX_UI_KEY_HANDLED;
}

static struct repeat_owner dispatch_event(struct fplinux_ui *ui,
					  const struct fplinux_ui_event *event)
{
	struct repeat_owner owner = { 0 };

	if (event->type == FPLINUX_UI_EVENT_INPUT_CANCEL)
		fplinux_ui_text_cancel_input(ui, event->key.source);
	if (event->type == FPLINUX_UI_EVENT_KEY) {
		enum fplinux_ui_key_route route;
		struct fplinux_ui_menu_view *menu = ui->menu;
		struct fplinux_ui_screen *screen;

		route = route_menu_key(ui, &event->key);
		if (route != FPLINUX_UI_KEY_UNHANDLED) {
			if (route == FPLINUX_UI_KEY_REPEATABLE)
				owner = (struct repeat_owner){
					.target = REPEAT_MENU,
					.menu = menu,
				};
			return owner;
		}
		screen = fplinux_ui_current(ui);
		route = fplinux_ui_screens_key(ui->screens, &event->key);
		if (route != FPLINUX_UI_KEY_UNHANDLED) {
			if (route == FPLINUX_UI_KEY_REPEATABLE)
				owner = (struct repeat_owner){
					.target = REPEAT_SCREEN,
					.screen = screen,
				};
			return owner;
		}
	}
	if (ui->callback) {
		struct text_key_claim claim = {
			.previous = ui->text_claim,
			.key = event->type == FPLINUX_UI_EVENT_KEY ?
				       &event->key :
				       NULL,
		};

		ui->text_claim = &claim;
		ui->callback(ui, event, ui->context);
		if (claim.field)
			owner = (struct repeat_owner){
				.target = REPEAT_RAW_TEXT,
				.field = claim.field,
			};
		ui->text_claim = claim.previous;
	}
	return owner;
}

static void set_lifecycle(struct fplinux_ui *ui,
			  enum fplinux_ui_lifecycle lifecycle)
{
	struct fplinux_ui_event event = { .type = FPLINUX_UI_EVENT_LIFECYCLE };

	if (ui->lifecycle == lifecycle)
		return;
	ui->lifecycle = lifecycle;
	dispatch_event(ui, &event);
}

static void request_close(struct fplinux_ui *ui)
{
	struct fplinux_ui_event event = { .type = FPLINUX_UI_EVENT_CLOSE };

	if (ui->quit)
		return;
	fplinux_ui_cancel_key_repeat(ui, NULL);
	set_lifecycle(ui, FPLINUX_UI_CLOSING);
	dispatch_event(ui, &event);
	ui->quit = true;
}

static const char *const close_response_names[] = { "ready", "cancel", "wait" };

static bool prepare_close(struct fplinux_ui *ui, uint32_t serial)
{
	enum fplinux_ui_close_response response;

	if (ui->close_serial == serial) {
		struct fplinux_ui_session_message reply = {
			.type = FPLINUX_UI_SESSION_CLOSE_REPLY,
			.serial = serial,
		};

		strcpy(reply.value, close_response_names[ui->close_response]);
		if (!fplinux_ui_session_send(ui->session_fd, &reply))
			return false;
		if (ui->close_response != FPLINUX_UI_CLOSE_READY)
			queue_frame_ack(ui, FPLINUX_UI_SESSION_CLOSE_ACK,
					serial);
		return true;
	}
	ui->close_serial = serial;
	ui->close_response = FPLINUX_UI_CLOSE_WAIT;
	/* Back held the last frame; a save decision must be visible and usable. */
	ui->exit_pending = false;
	fplinux_ui_cancel_key_repeat(ui, NULL);
	response = ui->close_callback ?
			   ui->close_callback(ui, ui->close_context) :
			   FPLINUX_UI_CLOSE_READY;
	return fplinux_ui_close_reply(ui, response);
}

/* [a-z]{2,3}, the name of a file in FPLINUX_LANGUAGE_DIRECTORY. */
static bool is_language_code(const char *code)
{
	size_t length = strspn(code, "abcdefghijklmnopqrstuvwxyz");

	return (length == 2 || length == 3) && !code[length];
}

/*
 * Messages follow the system language. The manager starts an application
 * with LANG from the language file; when the language changes while the
 * application runs, or LANG names another language, the file's LANG value
 * is used instead. musl translates LC_MESSAGES by that name, trying
 * "ru_RU" and then "ru" for "ru_RU.UTF-8".
 */
static void set_language(struct fplinux_ui *ui, const char *code)
{
	struct fplinux_language language;
	const char *messages;

	/* Start-up passes the code that CONFIG already stored here. */
	if (code != ui->locale)
		strcpy(ui->locale, code);
	if (!fplinux_language_load(FPLINUX_LANGUAGE_DIRECTORY, code,
				   &language) ||
	    !language.system) {
		fprintf(stderr,
			"fplinux-ui: system language %s unavailable; English\n",
			code);
		fplinux_language_english(&language);
	}
	messages = setlocale(LC_ALL, "");
	if (messages)
		messages = setlocale(LC_MESSAGES, NULL);
	if (!messages || strcmp(messages, language.lang))
		setlocale(LC_ALL, language.lang);
}

/* A runtime word, such as a soft-key legend, in the system language. */
static const char *word(enum fplinux_ui_label label)
{
	return dgettext(FPLINUX_UI_TEXT_DOMAIN, fplinux_ui_label(label));
}

/*
 * Animations, such as a list's marquee, wait while nothing is seen. LVGL
 * pauses its animation timer when the last animation ends and resumes it
 * whenever one starts, also while the surface is hidden; the main loop
 * therefore pauses it again before each LVGL pass.
 */
static void hold_animations(const struct fplinux_ui *ui)
{
	lv_timer_t *animations = lv_anim_get_timer();

	if (!animations)
		return;
	if (!ui->visible)
		lv_timer_pause(animations);
	else if (lv_anim_count_running())
		lv_timer_resume(animations);
}

/*
 * A hidden surface submits no frames and LVGL records no invalidation, so
 * nothing is rendered; showing it again redraws the whole screen once.
 */
static void set_visible(struct fplinux_ui *ui, bool visible)
{
	struct fplinux_ui_event event = {
		.type = FPLINUX_UI_EVENT_VISIBILITY,
		.visible = visible,
	};

	if (ui->visible == visible)
		return;
	/* Synthetic releases must observe that held gestures are now hidden. */
	ui->visible = visible;
	fplinux_ui_frames_set_visible(ui, visible);
	if (!visible)
		for (struct input_seat *seat = ui->seats; seat;
		     seat = seat->next)
			release_keys(seat);
	lv_display_enable_invalidation(ui->display, visible);
	if (visible)
		lv_obj_invalidate(lv_display_get_screen_active(ui->display));
	/* Without animations the timer stays paused: no idle wake-ups. */
	hold_animations(ui);
	fplinux_ui_screens_set_visible(ui->screens, visible);
	marquee_set_visible(ui, visible);
	dispatch_event(ui, &event);
}

static void queue_frame_ack(struct fplinux_ui *ui,
			    enum fplinux_ui_session_message_type type,
			    uint32_t serial)
{
	enum frame_ack_slot slot =
		type == FPLINUX_UI_SESSION_ACTIVE_ACK ? FRAME_ACK_ACTIVE :
		type == FPLINUX_UI_SESSION_ACTIVATION_ACK ?
							FRAME_ACK_ACTIVATION :
		type == FPLINUX_UI_SESSION_REDRAW_ACK ? FRAME_ACK_APPEARANCE :
							FRAME_ACK_CLOSE;
	struct frame_ack *ack = &ui->frame_acks[slot];

	if (ack->sync)
		wl_callback_destroy(ack->sync);
	*ack = (struct frame_ack){ .ui = ui, .type = type, .serial = serial };
	/* A previously queued canvas cannot acknowledge this new view. */
	ui->dirty = false;
	lv_obj_invalidate(lv_display_get_screen_active(ui->display));
}

static bool needs_frame_ack(const struct fplinux_ui *ui)
{
	for (unsigned int i = 0; i < FRAME_ACK_COUNT; ++i)
		if (ui->frame_acks[i].serial && !ui->frame_acks[i].sync)
			return true;
	return false;
}

static bool receive_session(struct fplinux_ui *ui)
{
	struct fplinux_ui_session_message message;
	struct fplinux_ui_event event = {
		.type = FPLINUX_UI_EVENT_SESSION,
		.session = &message,
	};
	int received;

	while ((received = fplinux_ui_session_receive(ui->session_fd,
						      &message)) > 0) {
		bool setting_reply =
			message.type == FPLINUX_UI_SESSION_SETTING ||
			(message.type == FPLINUX_UI_SESSION_ERROR &&
			 message.applied);

		/* These requests travel only from client to manager. */
		if (message.type == FPLINUX_UI_SESSION_LAUNCH ||
		    message.type == FPLINUX_UI_SESSION_ACTIVATION_ACK ||
		    message.type == FPLINUX_UI_SESSION_CLOSE_REPLY) {
			if (message.has_frame)
				close(message.frame_fd);
			errno = EPROTO;
			return false;
		}
		if (message.type == FPLINUX_UI_SESSION_VISIBILITY) {
			set_visible(ui, message.visible);
			continue;
		}
		if (message.type == FPLINUX_UI_SESSION_LIFECYCLE) {
			enum fplinux_ui_lifecycle lifecycle;

			switch (message.lifecycle) {
			case FPLINUX_UI_SESSION_FOREGROUND:
				lifecycle = FPLINUX_UI_FOREGROUND;
				break;
			case FPLINUX_UI_SESSION_BACKGROUND:
				lifecycle = FPLINUX_UI_BACKGROUND;
				break;
			case FPLINUX_UI_SESSION_CLOSING:
				lifecycle = FPLINUX_UI_CLOSING;
				break;
			default:
				errno = EPROTO;
				return false;
			}
			set_lifecycle(ui, lifecycle);
			continue;
		}
		if (message.type == FPLINUX_UI_SESSION_PREPARE_CLOSE) {
			if (!prepare_close(ui, message.serial))
				return false;
			continue;
		}
		if (message.type == FPLINUX_UI_SESSION_ACTIVATION) {
			struct fplinux_ui_event activation = {
				.type = FPLINUX_UI_EVENT_ACTIVATION,
				.activation =
					message.activation ==
							FPLINUX_UI_SESSION_ACTIVATION_OPEN ?
						FPLINUX_UI_ACTIVATION_OPEN :
						FPLINUX_UI_ACTIVATION_RESUME,
			};

			dispatch_event(ui, &activation);
			queue_frame_ack(ui, FPLINUX_UI_SESSION_ACTIVATION_ACK,
					message.serial);
			continue;
		}
		/*
		 * The settings app learns the applied locale from the reply,
		 * the launcher from ACTIVE; the application then redraws
		 * with gettext in the new language.
		 */
		if (setting_reply && !strcmp(message.name, "locale") &&
		    is_language_code(message.value) &&
		    strcmp(message.value, ui->locale))
			set_language(ui, message.value);
		if (setting_reply && !strcmp(message.name, "input_locale") &&
		    is_language_code(message.value))
			strcpy(ui->input_locale, message.value);
		/*
		 * The settings app also re-themes itself: the reply updates
		 * the theme in place and the application rebuilds its
		 * screens, whose styles keep the colours they were made with.
		 */
		if (setting_reply && !strcmp(message.name, "theme"))
			set_theme_kind(ui, message.value);
		if (message.type == FPLINUX_UI_SESSION_ACTIVE &&
		    is_language_code(message.locale) &&
		    strcmp(message.locale, ui->locale))
			set_language(ui, message.locale);
		if (message.type == FPLINUX_UI_SESSION_ACTIVE &&
		    is_language_code(message.input_locale))
			strcpy(ui->input_locale, message.input_locale);
		if (message.type == FPLINUX_UI_SESSION_CLOSE) {
			fprintf(stderr,
				"fplinux-ui: manager CLOSE reason=%s ready=%d\n",
				message.reason, ui->ready);
			request_close(ui);
		} else {
			dispatch_event(ui, &event);
		}
		if (message.serial &&
		    (message.type == FPLINUX_UI_SESSION_SETTING ||
		     message.type == FPLINUX_UI_SESSION_ERROR))
			queue_frame_ack(ui, FPLINUX_UI_SESSION_REDRAW_ACK,
					message.serial);
		/* The event callback borrows an attached scene for this call. */
		if (message.has_frame)
			close(message.frame_fd);
	}
	return received >= 0;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	struct frame_buffer *frame = data;

	(void)buffer;
	frame->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void frame_ack_done(void *data, struct wl_callback *callback,
			   uint32_t serial)
{
	struct frame_ack *pending = data;
	struct fplinux_ui *ui = pending->ui;
	struct fplinux_ui_session_message ack = {
		.type = pending->type,
		.serial = pending->serial,
	};

	(void)serial;
	wl_callback_destroy(callback);
	pending->sync = NULL;
	pending->serial = 0;
	if (!fplinux_ui_session_send(ui->session_fd, &ack))
		ui->failed = true;
}

static const struct wl_callback_listener frame_ack_listener = {
	.done = frame_ack_done,
};

static void destroy_buffers(struct frame_buffer *buffers)
{
	for (unsigned int i = 0; i < FPLINUX_UI_BUFFER_COUNT; ++i) {
		if (buffers[i].buffer)
			wl_buffer_destroy(buffers[i].buffer);
		if (buffers[i].pixels)
			munmap(buffers[i].pixels, buffers[i].bytes);
		buffers[i] = (struct frame_buffer){ 0 };
	}
}

static bool submit_frame(struct fplinux_ui *ui)
{
	struct frame_buffer *buffers = ui->buffer_sets[ui->buffer_set];
	struct frame_buffer *frame = NULL;
	size_t pixels = (size_t)ui->width * ui->height;

	/* The first frame is still submitted: READY depends on it. */
	if (ui->exit_pending || !ui->dirty || !ui->configured ||
	    (!ui->visible && ui->frame_submitted))
		return true;
	for (unsigned int i = 0; i < FPLINUX_UI_BUFFER_COUNT; ++i)
		if (!buffers[i].busy) {
			frame = &buffers[i];
			break;
		}
	if (!frame)
		return true;
	if (ui->has_rgb565) {
		memcpy(frame->pixels, ui->canvas, pixels * sizeof(uint16_t));
	} else {
		uint32_t *destination = frame->pixels;

		for (size_t i = 0; i < pixels; ++i) {
			uint16_t pixel = ui->canvas[i];
			uint32_t red = (pixel >> 11) & 31;
			uint32_t green = (pixel >> 5) & 63;
			uint32_t blue = pixel & 31;

			destination[i] =
				0xff000000U | ((red * 255 / 31) << 16) |
				((green * 255 / 63) << 8) | (blue * 255 / 31);
		}
	}
	frame->busy = true;
	ui->dirty = false;
	wl_surface_attach(ui->surface, frame->buffer, 0, 0);
	wl_surface_damage(ui->surface, 0, 0, ui->width, ui->height);
	/* The first buffer of a new size acknowledges its configure. */
	if (ui->ack_pending) {
		xdg_surface_ack_configure(ui->xdg_surface, ui->ack_serial);
		ui->ack_pending = false;
	}
	wl_surface_commit(ui->surface);
	for (unsigned int i = 0; i < FRAME_ACK_COUNT; ++i) {
		struct frame_ack *ack = &ui->frame_acks[i];

		if (!ack->serial || ack->sync)
			continue;
		/* sync is ordered after this commit on the same Wayland connection. */
		ack->sync = wl_display_sync(ui->wayland);
		if (!ack->sync ||
		    wl_callback_add_listener(ack->sync, &frame_ack_listener,
					     ack) < 0)
			return false;
	}
	/* The previous size's buffers are no longer the surface's content. */
	if (ui->old_buffers) {
		destroy_buffers(ui->buffer_sets[!ui->buffer_set]);
		ui->old_buffers = false;
	}
	if (!ui->frame_submitted)
		fprintf(stderr,
			"fplinux-ui: first SHM commit %dx%d format=%s\n",
			ui->width, ui->height,
			ui->has_rgb565 ? "RGB565" : "XRGB8888");
	ui->frame_submitted = true;
	return true;
}

static void flush_display(lv_display_t *display, const lv_area_t *area,
			  uint8_t *pixels)
{
	struct fplinux_ui *ui = lv_display_get_user_data(display);
	size_t row_pixels = (size_t)(area->x2 - area->x1 + 1);
	size_t stride = lv_display_get_buf_active(display)->header.stride / 2;
	const uint16_t *source = (const uint16_t *)pixels;

	for (int y = area->y1; y <= area->y2; ++y) {
		memcpy(ui->canvas + (size_t)y * ui->width + area->x1, source,
		       row_pixels * sizeof(*source));
		source += stride;
	}
	if (lv_display_flush_is_last(display))
		ui->dirty = true;
	lv_display_flush_ready(display);
}

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

/*
 * The xdg_surface.configure that applies the toplevel configure before it.
 * During fplinux_ui_open() it sets the initial buffer size and is acknowledged
 * at once. Later a new size within the display resizes everything now
 * and is acknowledged with the commit of the first buffer of that size; a
 * configure that arrives before that commit takes its place. A configure of
 * the current size is acknowledged at once unless a resize waits for its
 * commit.
 */
static void surface_configure(void *data, struct xdg_surface *surface,
			      uint32_t serial)
{
	struct fplinux_ui *ui = data;
	int width = ui->configure_width ? ui->configure_width : ui->width;
	int height = ui->configure_height ? ui->configure_height : ui->height;
	bool fullscreen = ui->configure_has_state ? ui->configure_fullscreen :
						    ui->fullscreen;

	if (width <= 0 || height <= 0 || width > ui->metrics.screen_width ||
	    height > ui->metrics.screen_height) {
		/* Unacknowledged, it binds nothing that is committed. */
		fprintf(stderr,
			"fplinux-ui: configure %dx%d outside the %dx%d display "
			"ignored\n",
			width, height, ui->metrics.screen_width,
			ui->metrics.screen_height);
		return;
	}
	if (!ui->display) {
		ui->width = width;
		ui->height = height;
		ui->fullscreen = fullscreen;
		xdg_surface_ack_configure(surface, serial);
		ui->configured = true;
		return;
	}
	if (width != ui->width || height != ui->height) {
		ui->ack_pending = true;
		ui->ack_serial = serial;
		resize(ui, width, height, fullscreen);
		return;
	}
	ui->fullscreen = fullscreen;
	if (ui->ack_pending)
		ui->ack_serial = serial;
	else
		xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener surface_listener = {
	.configure = surface_configure,
};

/* The size and state that the next xdg_surface.configure applies. */
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
			       int32_t width, int32_t height,
			       struct wl_array *states)
{
	struct fplinux_ui *ui = data;
	const uint32_t *state = states->data;
	size_t count = states->size / sizeof(*state);

	(void)toplevel;
	ui->configure_width = width;
	ui->configure_height = height;
	ui->configure_fullscreen = false;
	ui->configure_has_state = false;
	for (size_t i = 0; i < count; ++i)
		if (state[i] == XDG_TOPLEVEL_STATE_FULLSCREEN ||
		    state[i] == XDG_TOPLEVEL_STATE_MAXIMIZED) {
			ui->configure_has_state = true;
			ui->configure_fullscreen |=
				state[i] == XDG_TOPLEVEL_STATE_FULLSCREEN;
		}
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	(void)toplevel;
	fprintf(stderr, "fplinux-ui: compositor requested CLOSE\n");
	request_close(data);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
			    uint32_t format, int fd, uint32_t bytes)
{
	struct input_seat *seat = data;
	struct xkb_keymap *keymap = NULL;
	struct xkb_state *state = NULL;
	char *mapping;

	(void)keyboard;
	stop_repeat(seat);
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !bytes) {
		close(fd);
		return;
	}
	mapping = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (mapping == MAP_FAILED)
		return;
	if (mapping[bytes - 1] == '\0')
		keymap = xkb_keymap_new_from_string(seat->ui->xkb_context,
						    mapping,
						    XKB_KEYMAP_FORMAT_TEXT_V1,
						    0);
	munmap(mapping, bytes);
	if (keymap)
		state = xkb_state_new(keymap);
	if (!state) {
		xkb_keymap_unref(keymap);
		return;
	}
	xkb_state_unref(seat->key_state);
	xkb_keymap_unref(seat->keymap);
	seat->keymap = keymap;
	seat->key_state = state;
}

/* Wayland leave has no key releases; consumers still have to stop held input. */
static void release_keys(struct input_seat *seat)
{
	struct fplinux_ui_event cancelled = {
		.type = FPLINUX_UI_EVENT_INPUT_CANCEL,
		.key = { .source = seat->source },
	};

	stop_repeat(seat);
	dispatch_event(seat->ui, &cancelled);
	for (unsigned int code = 0; code < KEY_CNT; ++code) {
		unsigned char bit = (unsigned char)(1U << (code % 8));
		struct fplinux_ui_event event = {
			.type = FPLINUX_UI_EVENT_KEY,
			.key = {
				.source = seat->source,
				.code = code,
				.time_ms = fplinux_ui_now_ms(),
			},
		};

		if (!(seat->pressed[code / 8] & bit))
			continue;
		seat->pressed[code / 8] &= (unsigned char)~bit;
		dispatch_event(seat->ui, &event);
	}
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
			   uint32_t serial, struct wl_surface *surface,
			   struct wl_array *keys)
{
	struct input_seat *seat = data;

	(void)keyboard;
	(void)serial;
	(void)surface;
	(void)keys;
	seat->focused = surface == seat->ui->surface;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
			   uint32_t serial, struct wl_surface *surface)
{
	struct input_seat *seat = data;

	(void)keyboard;
	(void)serial;
	(void)surface;
	seat->focused = false;
	release_keys(seat);
}

static void resolve_keyboard_key(struct input_seat *seat,
				 struct fplinux_ui_key *key)
{
	const xkb_keysym_t *symbols;

	if (seat->source != FPLINUX_UI_KEY_KEYBOARD || !seat->key_state)
		return;
	key->keysym = xkb_state_key_get_one_sym(seat->key_state, key->code + 8);
	key->unicode = xkb_keysym_to_utf32(key->keysym);
	if (xkb_keymap_key_get_syms_by_level(seat->keymap, key->code + 8, 0, 0,
					     &symbols) == 1) {
		uint32_t ascii = xkb_keysym_to_utf32(symbols[0]);

		if (ascii > 0 && ascii < 128)
			key->ascii = ascii;
	}
	xkb_state_key_get_utf8(seat->key_state, key->code + 8, key->text,
			       sizeof(key->text));
	key->shift = xkb_state_mod_name_is_active(
		seat->key_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE);
	key->control = xkb_state_mod_name_is_active(
		seat->key_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);
	key->alt = xkb_state_mod_name_is_active(
		seat->key_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE);
}

static bool key_repeats(const struct input_seat *seat, unsigned int code)
{
	if (seat->source == FPLINUX_UI_KEY_PHONE)
		return code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT ||
		       code == KEY_RIGHT;
	return seat->keymap && xkb_keymap_key_repeats(seat->keymap, code + 8);
}

static bool same_repeat_owner(const struct repeat_owner *first,
			      const struct repeat_owner *second)
{
	if (first->target != second->target)
		return false;
	switch (first->target) {
	case REPEAT_MENU:
		return first->menu == second->menu;
	case REPEAT_SCREEN:
		return first->screen == second->screen;
	case REPEAT_RAW_TEXT:
		return first->field == second->field;
	case REPEAT_NONE:
		return false;
	}
	return false;
}

static bool repeat_owner_current(struct fplinux_ui *ui,
				 const struct repeat_owner *owner)
{
	switch (owner->target) {
	case REPEAT_MENU:
		return ui->menu == owner->menu;
	case REPEAT_SCREEN:
		return !ui->menu && fplinux_ui_current(ui) == owner->screen;
	case REPEAT_RAW_TEXT:
		return !ui->menu && !fplinux_ui_current(ui) &&
		       fplinux_ui_text_can_repeat(ui, owner->field);
	case REPEAT_NONE:
		return false;
	}
	return false;
}

static void repeat_key(lv_timer_t *timer)
{
	struct input_seat *seat = lv_timer_get_user_data(timer);
	struct fplinux_ui *ui = seat->ui;
	struct repeat_owner owner = seat->repeat_owner;
	struct repeat_owner delivered;
	uint64_t generation = seat->repeat_generation;
	unsigned char bit = (unsigned char)(1U << (seat->repeat_code % 8));
	struct fplinux_ui_event event = {
		.type = FPLINUX_UI_EVENT_KEY,
		.key = {
			.source = seat->source,
			.code = seat->repeat_code,
			.pressed = true,
			.time_ms = fplinux_ui_now_ms(),
		},
	};

	if (generation != ui->repeat_generation || !seat->focused ||
	    !ui->visible || ui->quit ||
	    !(seat->pressed[seat->repeat_code / 8] & bit) ||
	    !key_repeats(seat, seat->repeat_code) ||
	    !repeat_owner_current(ui, &owner)) {
		stop_repeat(seat);
		return;
	}
	resolve_keyboard_key(seat, &event.key);
	delivered = dispatch_event(ui, &event);
	if (generation != ui->repeat_generation ||
	    !same_repeat_owner(&owner, &seat->repeat_owner) ||
	    !same_repeat_owner(&owner, &delivered) ||
	    !repeat_owner_current(ui, &owner)) {
		stop_repeat(seat);
		return;
	}
	/* One event per wakeup; a delayed callback never catches up in a burst. */
	if (seat->repeat_rate > 0) {
		uint32_t period = seat->source == FPLINUX_UI_KEY_PHONE ?
					  200U :
					  1000U / (uint32_t)seat->repeat_rate;

		lv_timer_set_period(timer, period ? period : 1);
		lv_timer_reset(timer);
	}
}

static void start_repeat(struct input_seat *seat, unsigned int code,
			 const struct repeat_owner *owner)
{
	uint32_t delay;

	if (seat->repeat_rate <= 0)
		return;
	delay = seat->source == FPLINUX_UI_KEY_PHONE ? 500U :
		seat->repeat_delay > 0 ? (uint32_t)seat->repeat_delay :
					 1;
	if (!seat->repeat_timer) {
		seat->repeat_timer = lv_timer_create(repeat_key, delay, seat);
		if (!seat->repeat_timer) {
			seat->ui->failed = true;
			return;
		}
	}
	seat->repeat_owner = *owner;
	seat->repeat_generation = seat->ui->repeat_generation;
	seat->repeat_code = code;
	lv_timer_set_period(seat->repeat_timer, delay);
	lv_timer_reset(seat->repeat_timer);
	lv_timer_resume(seat->repeat_timer);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
			 uint32_t serial, uint32_t time_ms, uint32_t code,
			 uint32_t state)
{
	struct input_seat *seat = data;
	struct fplinux_ui *ui = seat->ui;
	bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	bool repeats;
	uint64_t generation;
	struct repeat_owner owner;
	unsigned char bit = (unsigned char)(1U << (code % 8));
	struct fplinux_ui_event event = {
		.type = FPLINUX_UI_EVENT_KEY,
		.key = {
			.source = seat->source,
			.code = code,
			.pressed = pressed,
			.time_ms = fplinux_ui_now_ms(),
		},
	};

	(void)keyboard;
	(void)serial;
	/* Consumers compare input deadlines with the runtime's monotonic clock. */
	(void)time_ms;
	if (!seat->focused || !ui->visible || code >= KEY_CNT)
		return;
	if (pressed == !!(seat->pressed[code / 8] & bit))
		return;
	repeats = key_repeats(seat, code);
	if ((pressed && repeats) || (!pressed && seat->repeat_code == code))
		stop_repeat(seat);
	if (pressed)
		seat->pressed[code / 8] |= bit;
	else
		seat->pressed[code / 8] &= (unsigned char)~bit;
	resolve_keyboard_key(seat, &event.key);
	generation = ui->repeat_generation;
	owner = dispatch_event(ui, &event);
	if (pressed && repeats && generation == ui->repeat_generation &&
	    repeat_owner_current(ui, &owner))
		start_repeat(seat, code, &owner);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
			       uint32_t serial, uint32_t depressed,
			       uint32_t latched, uint32_t locked,
			       uint32_t group)
{
	struct input_seat *seat = data;

	(void)keyboard;
	(void)serial;
	if (seat->key_state)
		xkb_state_update_mask(seat->key_state, depressed, latched,
				      locked, 0, 0, group);
}

static void keyboard_repeat(void *data, struct wl_keyboard *keyboard,
			    int32_t rate, int32_t delay)
{
	struct input_seat *seat = data;

	(void)keyboard;
	stop_repeat(seat);
	seat->repeat_rate = rate > 0 && delay >= 0 ? rate : 0;
	seat->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
			      uint32_t capabilities)
{
	struct input_seat *input = data;

	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !input->keyboard) {
		input->keyboard = wl_seat_get_keyboard(seat);
		if (!input->keyboard) {
			input->ui->failed = true;
			return;
		}
		wl_keyboard_add_listener(input->keyboard, &keyboard_listener,
					 input);
	} else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) &&
		   input->keyboard) {
		input->focused = false;
		release_keys(input);
		wl_keyboard_destroy(input->keyboard);
		input->keyboard = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	struct input_seat *input = data;

	(void)seat;
	input->source = !strcmp(name, FPLINUX_UI_KEYPAD_SEAT) ?
				FPLINUX_UI_KEY_PHONE :
				FPLINUX_UI_KEY_KEYBOARD;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
	struct fplinux_ui *ui = data;

	(void)shm;
	if (format == WL_SHM_FORMAT_RGB565)
		ui->has_rgb565 = true;
}

static const struct wl_shm_listener shm_listener = { .format = shm_format };

static void registry_global(void *data, struct wl_registry *registry,
			    uint32_t name, const char *interface,
			    uint32_t version)
{
	struct fplinux_ui *ui = data;

	if (!strcmp(interface, wl_compositor_interface.name)) {
		ui->compositor = wl_registry_bind(registry, name,
						  &wl_compositor_interface, 3);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		ui->shm =
			wl_registry_bind(registry, name, &wl_shm_interface, 1);
		wl_shm_add_listener(ui->shm, &shm_listener, ui);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		ui->wm = wl_registry_bind(registry, name,
					  &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(ui->wm, &wm_listener, ui);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		struct input_seat *seat = calloc(1, sizeof(*seat));

		if (!seat) {
			ui->failed = true;
			return;
		}
		seat->ui = ui;
		seat->global_name = name;
		seat->source = FPLINUX_UI_KEY_KEYBOARD;
		seat->seat = wl_registry_bind(registry, name,
					      &wl_seat_interface,
					      version < 5 ? version : 5);
		if (!seat->seat) {
			free(seat);
			ui->failed = true;
			return;
		}
		seat->next = ui->seats;
		ui->seats = seat;
		wl_seat_add_listener(seat->seat, &seat_listener, seat);
	}
}

static void destroy_seat(struct input_seat *seat)
{
	if (seat->repeat_timer)
		lv_timer_delete(seat->repeat_timer);
	if (seat->keyboard)
		wl_keyboard_destroy(seat->keyboard);
	wl_seat_destroy(seat->seat);
	xkb_state_unref(seat->key_state);
	xkb_keymap_unref(seat->keymap);
	free(seat);
}

static void registry_remove(void *data, struct wl_registry *registry,
			    uint32_t name)
{
	struct fplinux_ui *ui = data;
	struct input_seat **link = &ui->seats;

	(void)registry;
	while (*link) {
		struct input_seat *seat = *link;

		if (seat->global_name != name) {
			link = &seat->next;
			continue;
		}
		release_keys(seat);
		*link = seat->next;
		destroy_seat(seat);
		return;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static int create_buffers(struct fplinux_ui *ui, struct frame_buffer *buffers,
			  int width, int height)
{
	uint32_t format = ui->has_rgb565 ? WL_SHM_FORMAT_RGB565 :
					   WL_SHM_FORMAT_XRGB8888;
	int stride = width * (ui->has_rgb565 ? 2 : 4);

	for (unsigned int i = 0; i < FPLINUX_UI_BUFFER_COUNT; ++i) {
		struct frame_buffer *frame = &buffers[i];
		struct wl_shm_pool *pool;
		int fd = memfd_create("fplinux-ui-frame", MFD_CLOEXEC);
		int error;

		frame->bytes = (size_t)stride * height;
		if (fd < 0)
			return errno;
		if (ftruncate(fd, frame->bytes) < 0) {
			error = errno;
			close(fd);
			return error;
		}
		frame->pixels = mmap(NULL, frame->bytes, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, 0);
		if (frame->pixels == MAP_FAILED) {
			error = errno;
			frame->pixels = NULL;
			close(fd);
			return error;
		}
		pool = wl_shm_create_pool(ui->shm, fd, frame->bytes);
		close(fd);
		if (!pool)
			return ENOMEM;
		frame->buffer = wl_shm_pool_create_buffer(
			pool, 0, width, height, stride, format);
		wl_shm_pool_destroy(pool);
		if (!frame->buffer)
			return ENOMEM;
		wl_buffer_add_listener(frame->buffer, &buffer_listener, frame);
	}
	return 0;
}

/*
 * The canvas, a draw buffer for a new width and SHM buffers of the new
 * size, all allocated before anything changes. The buffers go to the free
 * set; a set that a previous resize made and never committed is replaced,
 * while the committed one keeps the surface's content.
 */
static bool allocate_size(struct fplinux_ui *ui, int width, int height,
			  uint16_t **canvas, void **draw_buffer)
{
	unsigned int set = ui->old_buffers ? ui->buffer_set : !ui->buffer_set;
	int error = ENOMEM;

	*canvas = calloc((size_t)width * height, sizeof(**canvas));
	*draw_buffer = NULL;
	if (width != ui->width)
		*draw_buffer = malloc((size_t)width * FPLINUX_UI_DRAW_ROWS *
				      sizeof(uint16_t));
	if (*canvas && (width == ui->width || *draw_buffer)) {
		destroy_buffers(ui->buffer_sets[set]);
		error = create_buffers(ui, ui->buffer_sets[set], width, height);
		if (!error) {
			ui->buffer_set = set;
			ui->old_buffers = true;
			return true;
		}
		destroy_buffers(ui->buffer_sets[set]);
	}
	free(*canvas);
	free(*draw_buffer);
	errno = error;
	return false;
}

/*
 * The surface takes a configured size: LVGL, the screen stack, an open
 * options menu and symbol grid are laid out again, the application hears
 * FPLINUX_UI_EVENT_RESIZE and the next frame redraws everything at the new
 * size. Running out of memory ends fplinux_ui_run() with an error.
 */
static void resize(struct fplinux_ui *ui, int width, int height,
		   bool fullscreen)
{
	struct fplinux_ui_event event = { .type = FPLINUX_UI_EVENT_RESIZE };
	uint16_t *canvas;
	void *draw_buffer;

	if (!allocate_size(ui, width, height, &canvas, &draw_buffer)) {
		fprintf(stderr, "fplinux-ui: resize to %dx%d failed: %s\n",
			width, height, strerror(errno));
		/* No old-size buffer may acknowledge the new size. */
		ui->ack_pending = false;
		ui->failed = true;
		return;
	}
	fprintf(stderr, "fplinux-ui: surface %dx%d -> %dx%d %s\n", ui->width,
		ui->height, width, height,
		fullscreen ? "fullscreen" : "integrated");
	free(ui->canvas);
	ui->canvas = canvas;
	ui->width = width;
	ui->height = height;
	ui->fullscreen = fullscreen;
	/* A frame drawn at the old size is not submitted any more. */
	ui->dirty = false;
	lv_display_set_resolution(ui->display, width, height);
	if (draw_buffer) {
		lv_display_set_buffers(ui->display, draw_buffer, NULL,
				       (size_t)width * FPLINUX_UI_DRAW_ROWS *
					       sizeof(uint16_t),
				       LV_DISPLAY_RENDER_MODE_PARTIAL);
		free(ui->draw_buffer);
		ui->draw_buffer = draw_buffer;
	}
	/* Committed with the first buffer of the new size. */
	fplinux_ui_screens_resize(ui->screens);
	fplinux_ui_menu_view_resize(ui->menu);
	marquee_restart(ui);
	dispatch_event(ui, &event);
	/* After the application's layout: a raw field may have moved. */
	fplinux_ui_text_resize(ui);
	show_legend_bar(ui);
	lv_obj_invalidate(lv_display_get_screen_active(ui->display));
}

static int wait_config(struct fplinux_ui *ui)
{
	uint64_t deadline_ms =
		fplinux_ui_now_ms() + FPLINUX_UI_CONFIG_TIMEOUT_MS;
	struct fplinux_ui_session_message config;
	struct pollfd descriptor = { .fd = ui->session_fd, .events = POLLIN };
	int received;

	while (fplinux_ui_now_ms() < deadline_ms) {
		received = fplinux_ui_session_receive(ui->session_fd, &config);
		if (received < 0)
			return EPROTO;
		if (received > 0) {
			if (config.has_frame) {
				close(config.frame_fd);
				return EPROTO;
			}
			if (config.type != FPLINUX_UI_SESSION_CONFIG ||
			    config.screen_width <= 0 ||
			    config.screen_height <= 0)
				return EPROTO;
			if (config.screen_width > FPLINUX_UI_SCREEN_MAX ||
			    config.screen_height > FPLINUX_UI_SCREEN_MAX)
				return EINVAL;
			ui->config_mode = config.mode;
			ui->audio = config.audio;
			ui->vibration = config.vibration;
			ui->requested_mode = config.mode;
			ui->fullscreen = config.mode ==
					 FPLINUX_UI_SESSION_FULLSCREEN;
			ui->metrics.screen_width = config.screen_width;
			ui->metrics.screen_height = config.screen_height;
			ui->theme.kind =
				config.theme == FPLINUX_UI_SESSION_THEME_DARK ?
					FPLINUX_UI_THEME_DARK :
					FPLINUX_UI_THEME_LIGHT;
			strcpy(ui->locale, is_language_code(config.locale) ?
						   config.locale :
						   "en");
			strcpy(ui->input_locale,
			       is_language_code(config.input_locale) ?
				       config.input_locale :
				       "en");
			return 0;
		}
		if (poll(&descriptor, 1,
			 (int)(deadline_ms - fplinux_ui_now_ms())) < 0) {
			int error = errno;

			if (error != EINTR)
				return error;
		}
	}
	return ETIMEDOUT;
}

static lv_color_t token(const struct fplinux_ui_theme_spec *palette,
			enum fplinux_ui_colour colour)
{
	return lv_color_hex(palette->rgb[colour]);
}

/*
 * The largest profile that fits the display CONFIG reported, and its
 * metrics. A display smaller than every profile gets the smallest one.
 */
static void set_metrics(struct fplinux_ui *ui)
{
	int screen_width = ui->metrics.screen_width;
	int screen_height = ui->metrics.screen_height;
	int profile =
		fplinux_ui_profile_for_screen(screen_width, screen_height);
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_profile_spec(profile);

	if (screen_width < spec->width || screen_height < spec->height)
		fprintf(stderr,
			"fplinux-ui: display %dx%d is smaller than %dx%d; "
			"layouts may not fit\n",
			screen_width, screen_height, spec->width, spec->height);
	ui->spec = spec;
	ui->metrics = (struct fplinux_ui_metrics){
		.profile = (enum fplinux_ui_profile)profile,
		.screen_width = screen_width,
		.screen_height = screen_height,
		.status_height = spec->status_height,
		.softkey_height = spec->softkey_height,
		.header_height = spec->header_height,
		.margin = spec->margin,
		.spacing = spec->spacing,
		.gap_small = spec->gap_small,
		.row_height = spec->row_height,
		.row_height_detail = spec->row_height_detail,
		.row_icon_size = spec->row_icon_size,
		.status_icon_size = spec->status_icon_size,
		.app_icon_size = spec->app_icon_size,
		.grid_columns = spec->grid_columns,
		.focus_width = spec->focus_width,
		.divider_width = spec->divider_width,
		.frame_padding = spec->frame_padding,
		.scrollbar_space = spec->scrollbar_space,
	};
}

/* The palette of the theme kind; the fonts stay. */
static void set_theme(struct fplinux_ui *ui)
{
	const struct fplinux_ui_theme_spec *palette =
		fplinux_ui_theme_spec(ui->theme.kind);
	struct fplinux_ui_theme *theme = &ui->theme;

	theme->background = token(palette, FPLINUX_UI_COLOUR_BACKGROUND);
	theme->surface = token(palette, FPLINUX_UI_COLOUR_SURFACE);
	theme->foreground = token(palette, FPLINUX_UI_COLOUR_FOREGROUND);
	theme->muted = token(palette, FPLINUX_UI_COLOUR_MUTED);
	theme->accent = token(palette, FPLINUX_UI_COLOUR_ACCENT);
	theme->accent_text = token(palette, FPLINUX_UI_COLOUR_ACCENT_TEXT);
	theme->border = token(palette, FPLINUX_UI_COLOUR_BORDER);
	theme->popup = token(palette, FPLINUX_UI_COLOUR_POPUP);
	theme->bar = token(palette, FPLINUX_UI_COLOUR_BAR);
	theme->bar_text = token(palette, FPLINUX_UI_COLOUR_BAR_TEXT);
	theme->disabled = token(palette, FPLINUX_UI_COLOUR_DISABLED);
	theme->focus = token(palette, FPLINUX_UI_COLOUR_FOCUS);
	theme->warning = token(palette, FPLINUX_UI_COLOUR_WARNING);
	theme->danger = token(palette, FPLINUX_UI_COLOUR_DANGER);
	theme->tile_ring = token(palette, FPLINUX_UI_COLOUR_TILE_RING);
	theme->brand = token(palette, FPLINUX_UI_COLOUR_BRAND);
	theme->scrim = token(palette, FPLINUX_UI_COLOUR_SCRIM);
	theme->scrim_opacity = palette->scrim_opacity;
}

/*
 * A SETTING reply with the new theme: "light" or "dark". The screen's own
 * colours follow at once; everything else is rebuilt by the application.
 */
static void set_theme_kind(struct fplinux_ui *ui, const char *value)
{
	enum fplinux_ui_theme_kind kind;
	lv_obj_t *screen;

	if (!strcmp(value, "light"))
		kind = FPLINUX_UI_THEME_LIGHT;
	else if (!strcmp(value, "dark"))
		kind = FPLINUX_UI_THEME_DARK;
	else
		return;
	if (kind == ui->theme.kind)
		return;
	ui->theme.kind = kind;
	set_theme(ui);
	screen = lv_display_get_screen_active(ui->display);
	lv_obj_set_style_bg_color(screen, ui->theme.background, 0);
	lv_obj_set_style_text_color(screen, ui->theme.foreground, 0);
}

/* The profile's font in each slot: an id of the profile table. */
static const char *slot_id(const struct fplinux_ui_profile_spec *spec,
			   enum font_slot slot)
{
	const char *ids[FONT_SLOTS] = {
		[FONT_BODY] = spec->font_body,
		[FONT_TITLE] = spec->font_title,
		[FONT_CLOCK] = spec->font_clock,
		[FONT_LEGEND] = spec->font_legend,
	};

	return ids[slot];
}

/* Resolve the profile's slot to a built-in font, or return an errno value. */
static int open_font(struct fplinux_ui *ui, enum font_slot slot)
{
	const char *id = slot_id(ui->spec, slot);

	ui->fonts[slot] = fplinux_ui_builtin_font(id);
	return ui->fonts[slot] ? 0 : EIO;
}

static int open_fonts(struct fplinux_ui *ui)
{
	int error = 0;

	for (int slot = 0; slot < FONT_SLOTS && !error; ++slot)
		error = open_font(ui, (enum font_slot)slot);
	if (error)
		return error;
	ui->theme.font = ui->fonts[FONT_BODY];
	ui->theme.font_large = ui->fonts[FONT_TITLE];
	ui->theme.font_clock = ui->fonts[FONT_CLOCK];
	ui->theme.font_softkey = ui->fonts[FONT_LEGEND];
	return 0;
}

struct fplinux_ui *fplinux_ui_open(const char *app_id,
				   fplinux_ui_event_cb callback, void *context)
{
	struct fplinux_ui *ui = calloc(1, sizeof(*ui));
	lv_obj_t *screen;
	int error = EIO;

	if (!ui)
		return NULL;
	ui->session_fd = -1;
	ui->watch_fd = -1;
	ui->callback = callback;
	ui->context = context;
	ui->visible = true;
	ui->session_fd = fplinux_ui_session_connect(app_id);
	if (ui->session_fd < 0) {
		error = errno;
		goto fail;
	}
	error = wait_config(ui);
	if (error)
		goto fail;
	set_metrics(ui);
	set_language(ui, ui->locale);
	/* musl translates bound domains only; English needs no catalogue. */
	if (!bindtextdomain(app_id, FPLINUX_UI_LOCALE_DIRECTORY) ||
	    !bindtextdomain(FPLINUX_UI_TEXT_DOMAIN,
			    FPLINUX_UI_LOCALE_DIRECTORY) ||
	    !textdomain(app_id))
		fprintf(stderr, "fplinux-ui: no translations: %s\n",
			strerror(errno));
	lv_init();
	lv_tick_set_cb(tick_ms);
	error = open_fonts(ui);
	if (error)
		goto fail;
	set_theme(ui);
	ui->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!ui->xkb_context) {
		error = ENOMEM;
		goto fail;
	}
	ui->wayland = wl_display_connect(NULL);
	if (!ui->wayland) {
		error = EIO;
		goto fail;
	}
	ui->registry = wl_display_get_registry(ui->wayland);
	if (!ui->registry) {
		error = ENOMEM;
		goto fail;
	}
	wl_registry_add_listener(ui->registry, &registry_listener, ui);
	if (wl_display_roundtrip(ui->wayland) < 0 ||
	    wl_display_roundtrip(ui->wayland) < 0) {
		error = EIO;
		goto fail;
	}
	if (ui->failed) {
		error = ENOMEM;
		goto fail;
	}
	if (!ui->compositor || !ui->shm || !ui->wm) {
		error = EPROTO;
		goto fail;
	}
	ui->surface = wl_compositor_create_surface(ui->compositor);
	if (!ui->surface) {
		error = ENOMEM;
		goto fail;
	}
	ui->xdg_surface = xdg_wm_base_get_xdg_surface(ui->wm, ui->surface);
	if (!ui->xdg_surface) {
		error = ENOMEM;
		goto fail;
	}
	xdg_surface_add_listener(ui->xdg_surface, &surface_listener, ui);
	ui->toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
	if (!ui->toplevel) {
		error = ENOMEM;
		goto fail;
	}
	xdg_toplevel_add_listener(ui->toplevel, &toplevel_listener, ui);
	xdg_toplevel_set_app_id(ui->toplevel, app_id);
	xdg_toplevel_set_title(ui->toplevel, app_id);
	wl_surface_commit(ui->surface);
	if (wl_display_roundtrip(ui->wayland) < 0) {
		error = EIO;
		goto fail;
	}
	if (!ui->configured) {
		error = EPROTO;
		goto fail;
	}
	error = create_buffers(ui, ui->buffer_sets[0], ui->width, ui->height);
	if (error)
		goto fail;
	ui->canvas = calloc((size_t)ui->width * ui->height, sizeof(uint16_t));
	ui->draw_buffer = malloc((size_t)ui->width * FPLINUX_UI_DRAW_ROWS *
				 sizeof(uint16_t));
	ui->display = lv_display_create(ui->width, ui->height);
	if (!ui->canvas || !ui->draw_buffer || !ui->display) {
		error = ENOMEM;
		goto fail;
	}
	lv_display_set_color_format(ui->display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(ui->display, ui->draw_buffer, NULL,
			       (size_t)ui->width * FPLINUX_UI_DRAW_ROWS *
				       sizeof(uint16_t),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_user_data(ui->display, ui);
	lv_display_set_flush_cb(ui->display, flush_display);
	lv_display_add_event_cb(ui->display, layout_done,
				LV_EVENT_UPDATE_LAYOUT_COMPLETED, ui);
	screen = lv_display_get_screen_active(ui->display);
	lv_obj_remove_style_all(screen);
	lv_obj_set_style_bg_color(screen, ui->theme.background, 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(screen, ui->theme.foreground, 0);
	lv_obj_set_style_text_font(screen, ui->theme.font, 0);
	lv_obj_set_scrollable(screen, false);
	stop_signal = 0;
	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	return ui;
fail:
	fplinux_ui_close(ui);
	errno = error;
	return NULL;
}

/*
 * Reads what the compositor sent and dispatches it without blocking.
 * wl_display_dispatch() polls again, with no timeout, when a read brings no
 * event to dispatch. After a resize the read can bring only the release of
 * a buffer that submit_frame() has destroyed, which libwayland discards,
 * while Weston holds back the wl_display.delete_id of that buffer until it
 * posts the next event, and only this loop's next commit would make it post
 * one: the application froze.
 */
static bool read_wayland(struct fplinux_ui *ui)
{
	if (wl_display_prepare_read(ui->wayland) == 0 &&
	    wl_display_read_events(ui->wayland) < 0)
		return false;
	return wl_display_dispatch_pending(ui->wayland) >= 0;
}

int fplinux_ui_run(struct fplinux_ui *ui)
{
	while (!ui->quit && !ui->failed) {
		struct pollfd descriptors[4] = {
			{ .fd = wl_display_get_fd(ui->wayland),
			  .events = POLLIN },
			{ .fd = ui->session_fd, .events = POLLIN },
			{ .fd = ui->watch_fd, .events = ui->watch_events },
			{ .fd = -1, .events = POLLIN },
		};
		uint32_t timeout_ms;
		int received;

		if (stop_signal) {
			request_close(ui);
			break;
		}
		if (wl_display_dispatch_pending(ui->wayland) < 0 ||
		    !receive_session(ui)) {
			ui->failed = true;
			break;
		}
		if (!ui->visible)
			hold_animations(ui);
		timeout_ms = lv_timer_handler();
		if (needs_frame_ack(ui) && !ui->exit_pending)
			lv_refr_now(ui->display);
		if (!submit_frame(ui)) {
			ui->failed = true;
			break;
		}
		if (ui->quit)
			break;
		if (wl_display_flush(ui->wayland) < 0) {
			if (errno != EAGAIN) {
				ui->failed = true;
				break;
			}
			descriptors[0].events |= POLLOUT;
		} else if (!ui->ready && ui->frame_submitted) {
			struct fplinux_ui_session_message ready = {
				.type = FPLINUX_UI_SESSION_READY,
			};

			if (!fplinux_ui_session_send(ui->session_fd, &ready)) {
				ui->failed = true;
				break;
			}
			ui->ready = true;
			fprintf(stderr,
				"fplinux-ui: READY sent after first Wayland flush\n");
		}
		/* LVGL refresh timers sleep between frames; no fixed busy polling. */
		descriptors[3].fd = fplinux_audio_client_fd(ui->audio_client);
		received = poll(
			descriptors, 4,
			timeout_ms == LV_NO_TIMER_READY ? -1 : (int)timeout_ms);
		if (received < 0) {
			if (errno == EINTR)
				continue;
			ui->failed = true;
			break;
		}
		if ((descriptors[0].revents | descriptors[1].revents) &
		    (POLLERR | POLLHUP | POLLNVAL)) {
			ui->failed = true;
			break;
		}
		if ((descriptors[0].revents & POLLIN) && !read_wayland(ui))
			ui->failed = true;
		if (descriptors[3].revents)
			fplinux_audio_client_dispatch(ui->audio_client,
						      descriptors[3].revents);
		if (descriptors[2].revents && ui->watch_callback)
			ui->watch_callback(ui, ui->watch_fd,
					   descriptors[2].revents,
					   ui->watch_context);
	}
	if (ui->failed)
		request_close(ui);
	return ui->failed ? -1 : 0;
}

void fplinux_ui_close(struct fplinux_ui *ui)
{
	if (!ui)
		return;
	fplinux_ui_frames_set_visible(ui, false);
	fplinux_audio_client_close(ui->audio_client);
	ui->audio_client = NULL;
	for (unsigned int i = 0; i < FRAME_ACK_COUNT; ++i)
		if (ui->frame_acks[i].sync)
			wl_callback_destroy(ui->frame_acks[i].sync);
	/* The application is leaving: no callbacks, legends or marquees. */
	ui->visible = false;
	ui->modals = 0;
	fplinux_ui_menu_view_delete(ui->menu);
	ui->menu = NULL;
	fplinux_ui_screens_delete(ui->screens);
	ui->screens = NULL;
	ui->marquee = (struct marquee){ 0 };
	if (ui->display)
		lv_display_delete(ui->display);
	free(ui->draw_buffer);
	free(ui->canvas);
	destroy_buffers(ui->buffer_sets[0]);
	destroy_buffers(ui->buffer_sets[1]);
	if (ui->toplevel)
		xdg_toplevel_destroy(ui->toplevel);
	if (ui->xdg_surface)
		xdg_surface_destroy(ui->xdg_surface);
	if (ui->surface)
		wl_surface_destroy(ui->surface);
	while (ui->seats) {
		struct input_seat *seat = ui->seats;

		ui->seats = seat->next;
		destroy_seat(seat);
	}
	if (ui->wm)
		xdg_wm_base_destroy(ui->wm);
	if (ui->shm)
		wl_shm_destroy(ui->shm);
	if (ui->compositor)
		wl_compositor_destroy(ui->compositor);
	if (ui->registry)
		wl_registry_destroy(ui->registry);
	if (ui->wayland)
		wl_display_disconnect(ui->wayland);
	xkb_context_unref(ui->xkb_context);
	while (ui->glyphs) {
		struct glyph *glyph = ui->glyphs;

		ui->glyphs = glyph->next;
		lv_draw_buf_destroy(glyph->image);
		free(glyph);
	}
	if (ui->session_fd >= 0)
		close(ui->session_fd);
	free(ui);
}

void fplinux_ui_quit(struct fplinux_ui *ui)
{
	fplinux_ui_cancel_key_repeat(ui, NULL);
	ui->quit = true;
	set_lifecycle(ui, FPLINUX_UI_CLOSING);
}

void fplinux_ui_set_close_handler(struct fplinux_ui *ui,
				  fplinux_ui_close_cb callback, void *context)
{
	ui->close_callback = callback;
	ui->close_context = context;
}

bool fplinux_ui_close_reply(struct fplinux_ui *ui,
			    enum fplinux_ui_close_response response)
{
	struct fplinux_ui_session_message reply = {
		.type = FPLINUX_UI_SESSION_CLOSE_REPLY,
		.serial = ui->close_serial,
	};

	if (!ui->close_serial || ui->close_response != FPLINUX_UI_CLOSE_WAIT ||
	    (unsigned int)response > FPLINUX_UI_CLOSE_WAIT || ui->quit) {
		errno = EINVAL;
		return false;
	}
	strcpy(reply.value, close_response_names[response]);
	if (!fplinux_ui_session_send(ui->session_fd, &reply))
		return false;
	ui->close_response = response;
	ui->exit_pending = response == FPLINUX_UI_CLOSE_READY;
	if (response != FPLINUX_UI_CLOSE_READY)
		queue_frame_ack(ui, FPLINUX_UI_SESSION_CLOSE_ACK,
				ui->close_serial);
	return true;
}

const char *fplinux_ui_storage_error(struct fplinux_storage_result result)
{
	const char *message;

	if (result.state == FPLINUX_STORAGE_DURABLE)
		return "";
	switch (result.error) {
	case ENOSPC:
		message = "Not enough storage space";
		break;
	case EDQUOT:
		message = "Storage limit reached";
		break;
	case EROFS:
		message = "Storage is read-only";
		break;
	case EACCES:
	case EPERM:
		message = "Storage access denied";
		break;
	case ENOMEM:
		message = "Not enough memory to save changes";
		break;
	case EIO:
		message = "Storage input/output error";
		break;
	default:
		message = "Could not save changes";
		break;
	}
	return dgettext(FPLINUX_UI_TEXT_DOMAIN, message);
}

void fplinux_ui_watch_fd(struct fplinux_ui *ui, int fd, short events,
			 fplinux_ui_fd_cb callback, void *context)
{
	ui->watch_fd = fd;
	ui->watch_events = events;
	ui->watch_callback = callback;
	ui->watch_context = context;
}

lv_obj_t *fplinux_ui_screen(struct fplinux_ui *ui)
{
	return lv_display_get_screen_active(ui->display);
}

int fplinux_ui_width(const struct fplinux_ui *ui)
{
	return ui->width;
}

int fplinux_ui_height(const struct fplinux_ui *ui)
{
	return ui->height;
}

const struct fplinux_ui_theme *fplinux_ui_theme(const struct fplinux_ui *ui)
{
	return &ui->theme;
}

const struct fplinux_ui_metrics *fplinux_ui_metrics(const struct fplinux_ui *ui)
{
	return &ui->metrics;
}

const struct fplinux_ui_profile_spec *
fplinux_ui_spec(const struct fplinux_ui *ui)
{
	return ui->spec;
}

bool fplinux_ui_is_launcher(const struct fplinux_ui *ui)
{
	return ui->config_mode == FPLINUX_UI_SESSION_LAUNCHER;
}

/* Glyph files are named [a-z0-9-]+, as "screen-timeout". */
static bool is_glyph_name(const char *name)
{
	size_t length = strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-");

	return length && length < FPLINUX_UI_GLYPH_NAME_BYTES && !name[length];
}

/*
 * /usr/share/fplinux/glyphs/<size>/<name>.png at the profile's row or
 * status icon size, decoded once as an alpha mask and kept until close.
 */
const void *fplinux_ui_glyph(struct fplinux_ui *ui, const char *name,
			     enum fplinux_ui_glyph_size size)
{
	char path[PATH_MAX];
	struct glyph *glyph;
	int pixels;

	if (!ui || !name || (unsigned int)size > FPLINUX_UI_GLYPH_STATUS) {
		errno = EINVAL;
		return NULL;
	}
	if (!is_glyph_name(name)) {
		errno = ENOENT;
		return NULL;
	}
	pixels = size == FPLINUX_UI_GLYPH_ROW ? ui->spec->row_icon_size :
						ui->spec->status_icon_size;
	for (glyph = ui->glyphs; glyph; glyph = glyph->next)
		if (glyph->size == pixels && !strcmp(glyph->name, name))
			return glyph->image;
	snprintf(path, sizeof(path), "/usr/share/fplinux/glyphs/%d/%s.png",
		 pixels, name);
	glyph = calloc(1, sizeof(*glyph));
	if (!glyph)
		return NULL;
	glyph->image = fplinux_ui_image_decode(path, pixels, pixels, true);
	if (!glyph->image) {
		int error = errno;

		free(glyph);
		errno = error;
		return NULL;
	}
	glyph->size = pixels;
	strcpy(glyph->name, name);
	glyph->next = ui->glyphs;
	ui->glyphs = glyph;
	return glyph->image;
}

bool fplinux_ui_send(struct fplinux_ui *ui,
		     const struct fplinux_ui_session_message *message)
{
	if (message->type == FPLINUX_UI_SESSION_LIFECYCLE ||
	    message->type == FPLINUX_UI_SESSION_ACTIVATION ||
	    message->type == FPLINUX_UI_SESSION_ACTIVATION_ACK ||
	    message->type == FPLINUX_UI_SESSION_PREPARE_CLOSE) {
		errno = EINVAL;
		return false;
	}
	if (message->type == FPLINUX_UI_SESSION_ACTIVE_ACK) {
		if (ui->config_mode != FPLINUX_UI_SESSION_LAUNCHER ||
		    !message->serial) {
			errno = EINVAL;
			return false;
		}
		queue_frame_ack(ui, message->type, message->serial);
		return true;
	}
	if (!fplinux_ui_session_send(ui->session_fd, message))
		return false;
	if (message->type == FPLINUX_UI_SESSION_EXIT)
		ui->exit_pending = true;
	return true;
}

/* The session protocol rejects control characters in every text field. */
static bool has_control_character(const char *text)
{
	for (; *text; ++text)
		if ((unsigned char)*text < 0x20 || *text == 0x7f)
			return true;
	return false;
}

bool fplinux_ui_set_title(struct fplinux_ui *ui, const char *title)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_TITLE,
	};

	if (strlen(title) >= sizeof(message.title)) {
		errno = ENAMETOOLONG;
		return false;
	}
	if (has_control_character(title)) {
		errno = EINVAL;
		return false;
	}
	strcpy(message.title, title);
	xdg_toplevel_set_title(ui->toplevel, title);
	return fplinux_ui_send(ui, &message);
}

bool fplinux_ui_set_softkeys(struct fplinux_ui *ui, const char *left,
			     const char *centre, const char *right)
{
	if (strlen(left) >= sizeof(ui->softkeys[0]) ||
	    strlen(centre) >= sizeof(ui->softkeys[1]) ||
	    strlen(right) >= sizeof(ui->softkeys[2])) {
		errno = ENAMETOOLONG;
		return false;
	}
	if (has_control_character(left) || has_control_character(centre) ||
	    has_control_character(right)) {
		errno = EINVAL;
		return false;
	}
	strcpy(ui->softkeys[0], left);
	strcpy(ui->softkeys[1], centre);
	strcpy(ui->softkeys[2], right);
	/* An open menu keeps its legends; closing it shows these. */
	if (ui->menu)
		return true;
	return send_softkeys(ui, left, centre, right);
}

lv_obj_t *fplinux_ui_header(struct fplinux_ui *ui, const char *title)
{
	const struct fplinux_ui_profile_spec *spec = ui->spec;
	lv_obj_t *band = lv_obj_create(fplinux_ui_screen(ui));
	lv_obj_t *label;

	if (!band)
		return NULL;
	lv_obj_remove_style_all(band);
	lv_obj_set_pos(band, 0, 0);
	lv_obj_set_size(band, ui->width, spec->header_height);
	lv_obj_set_style_bg_color(band, ui->theme.surface, 0);
	lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(band, ui->theme.border, 0);
	lv_obj_set_style_border_width(band, spec->divider_width, 0);
	lv_obj_set_style_border_side(band, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_scrollable(band, false);
	label = lv_label_create(band);
	if (!label) {
		lv_obj_delete(band);
		return NULL;
	}
	fplinux_ui_title_init(label, ui->theme.font_large);
	lv_obj_set_style_text_color(label, ui->theme.foreground, 0);
	lv_obj_set_width(label, ui->width - 2 * spec->margin);
	lv_label_set_text(label, title);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, spec->margin, 0);
	fplinux_ui_title_scroll(label);
	return band;
}

/* Marquee */

static void title_done(lv_anim_t *animation);
static void turn_done(lv_anim_t *animation);

/*
 * Every marquee copies its timing from one of these: a title's one pass,
 * the focused element's first pass and every later one.
 */
static lv_anim_t title_pass;
static lv_anim_t first_pass;
static lv_anim_t next_pass;

static void init_passes(void)
{
	if (title_pass.completed_cb)
		return;
	lv_anim_init(&title_pass);
	lv_anim_set_delay(&title_pass, MARQUEE_DELAY_MS);
	lv_anim_set_reverse_delay(&title_pass, MARQUEE_PAUSE_MS);
	lv_anim_set_repeat_count(&title_pass, 1);
	lv_anim_set_completed_cb(&title_pass, title_done);
	first_pass = title_pass;
	lv_anim_set_completed_cb(&first_pass, turn_done);
	next_pass = first_pass;
	lv_anim_set_delay(&next_pass, MARQUEE_PAUSE_MS);
}

static struct fplinux_ui *ui_of(const lv_obj_t *label)
{
	return lv_display_get_user_data(lv_obj_get_display(label));
}

static void rest(lv_obj_t *label)
{
	if (lv_label_get_long_mode(label) != LV_LABEL_LONG_MODE_DOTS)
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

/* The label scrolls once to its end and back, then calls back. */
static void arm(lv_obj_t *label, const lv_anim_t *pass)
{
	uint32_t speed = lv_anim_speed_clamped(MARQUEE_SPEED_PX_S,
					       MARQUEE_MIN_MS, MARQUEE_MAX_MS);

	if (lv_obj_get_style_anim(label, LV_PART_MAIN) != pass)
		lv_obj_set_style_anim(label, pass, 0);
	if (lv_obj_get_style_anim_duration(label, LV_PART_MAIN) != speed)
		lv_obj_set_style_anim_duration(label, speed, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL);
}

static int32_t text_width(lv_obj_t *label)
{
	lv_point_t size;

	lv_text_get_size(&size, lv_label_get_text(label),
			 lv_obj_get_style_text_font(label, LV_PART_MAIN),
			 lv_obj_get_style_text_letter_space(label,
							    LV_PART_MAIN),
			 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return size.x;
}

/* The text ends with LV_LABEL_DOT_NUM dots. */
static bool ends_with_dots(const char *text)
{
	size_t length = strlen(text);

	if (length < LV_LABEL_DOT_NUM)
		return false;
	for (size_t i = length - LV_LABEL_DOT_NUM; i < length; ++i)
		if (text[i] != '.')
			return false;
	return true;
}

/*
 * The label's whole text fits width on one line. A label that rested with
 * dots keeps them in its text, over the letters they hide, until LVGL
 * refreshes it, later than layout_done() or a title shown again measure
 * it. A text that ends with dots and fits is set again: that puts the
 * letters back, and the whole text is measured.
 */
static bool text_fits(lv_obj_t *label, int32_t width)
{
	if (text_width(label) > width)
		return false;
	if (!ends_with_dots(lv_label_get_text(label)))
		return true;
	lv_label_set_text(label, NULL);
	return text_width(label) <= width;
}

static void label_deleted(lv_event_t *event);

static void watch(struct fplinux_ui *ui, lv_obj_t *label)
{
	lv_obj_add_event_cb(label, label_deleted, LV_EVENT_DELETE, ui);
}

static void unwatch(struct fplinux_ui *ui, lv_obj_t *label)
{
	lv_obj_remove_event_cb_with_user_data(label, label_deleted, ui);
}

/*
 * The next label from start, in turn order, not known to fit; count when
 * every label fits.
 */
static unsigned int next_turn(const struct marquee *marquee, unsigned int start,
			      unsigned int fitting)
{
	for (unsigned int n = 0; n < marquee->count; ++n) {
		unsigned int turn = (start + n) % marquee->count;

		if (!(fitting & 1U << turn))
			return turn;
	}
	return marquee->count;
}

static void start_turn(struct fplinux_ui *ui, unsigned int start)
{
	struct marquee *marquee = &ui->marquee;
	unsigned int turn = next_turn(marquee, start, marquee->fitting);

	if (marquee->armed && turn != marquee->turn)
		rest(marquee->labels[marquee->turn]);
	marquee->armed = false;
	if (turn == marquee->count)
		return;
	marquee->turn = turn;
	marquee->checked = false;
	marquee->armed = true;
	arm(marquee->labels[turn], marquee->passed ? &next_pass : &first_pass);
}

/* The focused element's marquee starts unless a title or hiding holds it. */
static void begin(struct fplinux_ui *ui)
{
	struct marquee *marquee = &ui->marquee;

	if (!ui->visible || marquee->title || !marquee->count)
		return;
	marquee->fitting = 0;
	marquee->passed = false;
	start_turn(ui, 0);
}

static void pause_focus(struct marquee *marquee)
{
	if (marquee->armed)
		rest(marquee->labels[marquee->turn]);
	marquee->armed = false;
}

static void title_done(lv_anim_t *animation)
{
	lv_obj_t *label = animation->var;
	struct fplinux_ui *ui = ui_of(label);

	rest(label);
	if (ui->marquee.title != label)
		return;
	unwatch(ui, label);
	ui->marquee.title = NULL;
	begin(ui);
}

static void turn_done(lv_anim_t *animation)
{
	lv_obj_t *label = animation->var;
	struct fplinux_ui *ui = ui_of(label);
	struct marquee *marquee = &ui->marquee;

	if (!marquee->armed || marquee->labels[marquee->turn] != label) {
		rest(label);
		return;
	}
	marquee->passed = true;
	start_turn(ui, marquee->turn + 1);
}

/*
 * A focused label is measured once laid out: one that fits gives its turn
 * to the next, so that a short text never holds back a long detail.
 */
static void layout_done(lv_event_t *event)
{
	struct fplinux_ui *ui = lv_event_get_user_data(event);
	struct marquee *marquee = &ui->marquee;
	lv_obj_t *label;
	int32_t width;

	if (!marquee->armed || marquee->checked)
		return;
	label = marquee->labels[marquee->turn];
	width = lv_obj_get_content_width(label);
	if (width <= 0)
		return;
	marquee->checked = true;
	if (text_fits(label, width)) {
		marquee->fitting |= 1U << marquee->turn;
		start_turn(ui, marquee->turn + 1);
	}
}

static void forget_focus(struct fplinux_ui *ui, lv_obj_t *deleted)
{
	struct marquee *marquee = &ui->marquee;

	for (unsigned int i = 0; i < marquee->count; ++i) {
		if (marquee->labels[i] == deleted)
			continue;
		unwatch(ui, marquee->labels[i]);
		rest(marquee->labels[i]);
	}
	marquee->labels[0] = NULL;
	marquee->labels[1] = NULL;
	marquee->count = 0;
	marquee->armed = false;
}

/* label is one of the focused element's labels. */
static bool is_focus_label(const struct marquee *marquee, const lv_obj_t *label)
{
	for (unsigned int i = 0; i < marquee->count; ++i)
		if (label && marquee->labels[i] == label)
			return true;
	return false;
}

static void label_deleted(lv_event_t *event)
{
	struct fplinux_ui *ui = lv_event_get_user_data(event);
	struct marquee *marquee = &ui->marquee;
	lv_obj_t *label = lv_event_get_target(event);

	if (is_focus_label(marquee, label))
		forget_focus(ui, label);
	if (marquee->title == label) {
		marquee->title = NULL;
		begin(ui);
	}
}

void fplinux_ui_marquee_focus(struct fplinux_ui *ui, lv_obj_t *first,
			      lv_obj_t *second)
{
	struct marquee *marquee = &ui->marquee;
	lv_obj_t *labels[2] = { first ? first : second, first ? second : NULL };
	unsigned int count = labels[1] ? 2 : labels[0] ? 1 : 0;

	if (count == marquee->count &&
	    (!count || labels[0] == marquee->labels[0]) &&
	    (count < 2 || labels[1] == marquee->labels[1])) {
		/* Still the focus: a resting marquee measures again. */
		if (!marquee->armed)
			begin(ui);
		return;
	}
	for (unsigned int i = 0; i < marquee->count; ++i) {
		if (marquee->labels[i] == labels[0] ||
		    marquee->labels[i] == labels[1])
			continue;
		unwatch(ui, marquee->labels[i]);
		rest(marquee->labels[i]);
	}
	for (unsigned int i = 0; i < count; ++i)
		if (!is_focus_label(marquee, labels[i]))
			watch(ui, labels[i]);
	marquee->labels[0] = labels[0];
	marquee->labels[1] = labels[1];
	marquee->count = count;
	marquee->armed = false;
	if (count > 1)
		rest(labels[1]);
	if (count && (marquee->title || !ui->visible))
		rest(labels[0]);
	begin(ui);
}

void fplinux_ui_marquee_changed(struct fplinux_ui *ui, lv_obj_t *first,
				lv_obj_t *second)
{
	struct marquee *marquee = &ui->marquee;

	if (!is_focus_label(marquee, first))
		return;
	fplinux_ui_marquee_focus(ui, first, second);
	pause_focus(marquee);
	begin(ui);
}

void fplinux_ui_marquee_blur(struct fplinux_ui *ui, lv_obj_t *label)
{
	struct marquee *marquee = &ui->marquee;

	if (is_focus_label(marquee, label))
		forget_focus(ui, NULL);
}

/* Hidden: nothing moves. Shown again: every marquee starts over. */
static void marquee_set_visible(struct fplinux_ui *ui, bool visible)
{
	struct marquee *marquee = &ui->marquee;

	if (!visible) {
		if (marquee->title)
			rest(marquee->title);
		pause_focus(marquee);
	} else if (marquee->title) {
		arm(marquee->title, &title_pass);
	} else {
		begin(ui);
	}
}

/* New widths: the focused element's labels are measured again. */
static void marquee_restart(struct fplinux_ui *ui)
{
	pause_focus(&ui->marquee);
	begin(ui);
}

void fplinux_ui_title_init(lv_obj_t *label, const lv_font_t *font)
{
	init_passes();
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_height(label, lv_font_get_line_height(font));
	lv_obj_set_style_anim(label, &title_pass, 0);
	lv_obj_set_style_anim_duration(label,
				       lv_anim_speed_clamped(MARQUEE_SPEED_PX_S,
							     MARQUEE_MIN_MS,
							     MARQUEE_MAX_MS),
				       0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

/* The title stops its pass and lets the focused element's marquee run. */
static void release_title(struct fplinux_ui *ui)
{
	struct marquee *marquee = &ui->marquee;

	if (!marquee->title)
		return;
	unwatch(ui, marquee->title);
	rest(marquee->title);
	marquee->title = NULL;
	begin(ui);
}

void fplinux_ui_title_hide(lv_obj_t *label)
{
	struct fplinux_ui *ui = ui_of(label);

	if (ui->marquee.title == label)
		release_title(ui);
}

/*
 * The title now shown: one that fits rests at once, a longer one makes its
 * pass now and holds the focused element's marquee until it is back with
 * dots. A title shown before gives up its pass either way.
 */
void fplinux_ui_title_scroll(lv_obj_t *label)
{
	struct fplinux_ui *ui = ui_of(label);
	struct marquee *marquee = &ui->marquee;
	int32_t width = lv_obj_get_style_width(label, LV_PART_MAIN);

	init_passes();
	if (LV_COORD_IS_SPEC(width))
		width = lv_obj_get_content_width(label);
	if (text_fits(label, width)) {
		rest(label);
		release_title(ui);
		return;
	}
	if (marquee->title && marquee->title != label) {
		unwatch(ui, marquee->title);
		rest(marquee->title);
		marquee->title = NULL;
	}
	if (marquee->title != label) {
		marquee->title = label;
		watch(ui, label);
	}
	pause_focus(marquee);
	if (ui->visible)
		arm(label, &title_pass);
}

bool fplinux_ui_back(struct fplinux_ui *ui)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_EXIT,
	};

	return fplinux_ui_send(ui, &message);
}

bool fplinux_ui_home(struct fplinux_ui *ui)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_HOME,
	};

	fplinux_ui_cancel_key_repeat(ui, NULL);
	return fplinux_ui_send(ui, &message);
}

bool fplinux_ui_keep_screen_on(struct fplinux_ui *ui, bool enabled)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_KEEP_SCREEN_ON,
		.enabled = enabled,
	};

	return fplinux_ui_send(ui, &message);
}

bool fplinux_ui_audio_supported(const struct fplinux_ui *ui)
{
	return ui->audio;
}

bool fplinux_ui_audio_background_supported(const struct fplinux_ui *ui)
{
	(void)ui;
	return false;
}

static bool media_eligible(const struct fplinux_ui *ui)
{
	return ui->visible && !ui->quit && !ui->exit_pending &&
	       ui->lifecycle == FPLINUX_UI_FOREGROUND;
}

struct fplinux_audio_stream *fplinux_ui_audio_open(struct fplinux_ui *ui,
						   unsigned int rate,
						   unsigned int channels)
{
	if (!fplinux_audio_format_valid(rate, channels)) {
		errno = EINVAL;
		return NULL;
	}
	if (!ui->audio) {
		errno = ENOTSUP;
		return NULL;
	}
	if (!media_eligible(ui)) {
		errno = EAGAIN;
		return NULL;
	}
	if (!ui->audio_client) {
		ui->audio_client = fplinux_audio_client_create();
		if (!ui->audio_client)
			return NULL;
	}
	return fplinux_audio_stream_open(ui->audio_client, rate, channels);
}

long fplinux_ui_audio_write(struct fplinux_audio_stream *stream,
			    const int16_t *frames, size_t count)
{
	return fplinux_audio_stream_write(stream, frames, count);
}

long fplinux_ui_audio_queued(const struct fplinux_audio_stream *stream)
{
	return fplinux_audio_stream_queued(stream);
}

void fplinux_ui_audio_close(struct fplinux_audio_stream *stream)
{
	fplinux_audio_stream_close(stream);
}

bool fplinux_ui_vibration_supported(const struct fplinux_ui *ui)
{
	return ui->vibration;
}

bool fplinux_ui_vibration_play(struct fplinux_ui *ui,
			       const unsigned int *pattern_ms, size_t count,
			       bool repeat)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_VIBRATE,
		.repeat = repeat,
	};

	if (pattern_ms && count && count <= FPLINUX_UI_SESSION_PATTERN) {
		memcpy(message.pattern, pattern_ms,
		       count * sizeof(*pattern_ms));
		message.pattern_count = count;
	}
	if (!message.pattern_count ||
	    !fplinux_ui_session_vibrate_valid(&message)) {
		errno = EINVAL;
		return false;
	}
	if (!ui->vibration) {
		errno = ENOTSUP;
		return false;
	}
	if (!media_eligible(ui)) {
		errno = EAGAIN;
		return false;
	}
	return fplinux_ui_send(ui, &message);
}

bool fplinux_ui_vibration_stop(struct fplinux_ui *ui)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_VIBRATE,
	};

	return fplinux_ui_send(ui, &message);
}

bool fplinux_ui_is_visible(const struct fplinux_ui *ui)
{
	return ui->visible;
}

enum fplinux_ui_lifecycle fplinux_ui_lifecycle(const struct fplinux_ui *ui)
{
	return ui->lifecycle;
}

/* The manager answers MODE with a configure of the new size (resize()). */
bool fplinux_ui_set_fullscreen(struct fplinux_ui *ui, bool fullscreen)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_MODE,
		.mode = fullscreen ? FPLINUX_UI_SESSION_FULLSCREEN :
				     FPLINUX_UI_SESSION_INTEGRATED,
	};

	if (ui->config_mode == FPLINUX_UI_SESSION_LAUNCHER) {
		errno = EINVAL;
		return false;
	}
	if (message.mode == ui->requested_mode)
		return true;
	if (!fplinux_ui_send(ui, &message))
		return false;
	ui->requested_mode = message.mode;
	return true;
}

bool fplinux_ui_is_fullscreen(const struct fplinux_ui *ui)
{
	return ui->fullscreen;
}

const char *fplinux_ui_locale(const struct fplinux_ui *ui)
{
	return ui->locale;
}

const char *fplinux_ui_input_locale(const struct fplinux_ui *ui)
{
	return ui->input_locale;
}

bool fplinux_ui_is_softkey(const struct fplinux_ui_key *key, bool left)
{
	return key->source == FPLINUX_UI_KEY_PHONE &&
	       key->code == (left ? KEY_F13 : KEY_F14);
}

static bool state_directory_path(const char *app_id, char *path,
				 size_t capacity)
{
	const char *root = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	char base[PATH_MAX];

	if (!app_id || !app_id[0] || strchr(app_id, '/') ||
	    !strcmp(app_id, ".") || !strcmp(app_id, "..")) {
		errno = EINVAL;
		return false;
	}
	if (root && root[0] == '/') {
		if (snprintf(base, sizeof(base), "%s", root) >=
		    (int)sizeof(base)) {
			errno = ENAMETOOLONG;
			return false;
		}
	} else {
		if (!home || home[0] != '/') {
			errno = EINVAL;
			return false;
		}
		if (snprintf(base, sizeof(base), "%s/.local/state", home) >=
		    (int)sizeof(base)) {
			errno = ENAMETOOLONG;
			return false;
		}
	}
	if (snprintf(path, capacity, "%s/fplinux/%s", base, app_id) >=
	    (int)capacity) {
		errno = ENAMETOOLONG;
		return false;
	}
	return true;
}

bool fplinux_ui_state_dir(const char *app_id, char *path, size_t capacity)
{
	struct stat status;
	size_t length;

	if (!state_directory_path(app_id, path, capacity))
		return false;
	length = strlen(path);
	for (size_t i = 1; i <= length; ++i) {
		char saved = path[i];

		if (saved && saved != '/')
			continue;
		path[i] = '\0';
		if (mkdir(path, 0700) < 0 && errno != EEXIST) {
			path[i] = saved;
			return false;
		}
		if (stat(path, &status) < 0 || !S_ISDIR(status.st_mode)) {
			path[i] = saved;
			errno = ENOTDIR;
			return false;
		}
		path[i] = saved;
	}
	return true;
}

struct fplinux_ui_prefs *fplinux_ui_prefs_open(const char *app_id)
{
	char directory[PATH_MAX];

	if (!state_directory_path(app_id, directory, sizeof(directory)))
		return NULL;
	return fplinux_ui_prefs_open_directory(directory);
}

/* A top-layer popup owns its scrim, even when their parent deletes both. */
struct modal_scrim {
	lv_obj_t *object;
};

static void scrim_delete_cb(lv_event_t *event)
{
	struct modal_scrim *scrim = lv_event_get_user_data(event);

	scrim->object = NULL;
}

static void modal_delete_cb(lv_event_t *event)
{
	struct modal_scrim *scrim = lv_event_get_user_data(event);

	if (scrim->object)
		lv_obj_delete(scrim->object);
	free(scrim);
}

void fplinux_ui_scrim_style(struct fplinux_ui *ui, lv_obj_t *root)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);

	lv_obj_set_style_bg_color(root, theme->scrim, 0);
	lv_obj_set_style_bg_opa(root, theme->scrim_opacity, 0);
}

bool fplinux_ui_scrim_attach(struct fplinux_ui *ui, lv_obj_t *popup)
{
	struct modal_scrim *scrim = calloc(1, sizeof(*scrim));

	if (!scrim)
		return false;
	scrim->object = lv_obj_create(lv_obj_get_parent(popup));
	lv_obj_remove_style_all(scrim->object);
	lv_obj_set_scrollable(scrim->object, false);
	lv_obj_set_pos(scrim->object, 0, 0);
	/* The display's top layer: it follows a resized surface. */
	lv_obj_set_size(scrim->object, lv_pct(100), lv_pct(100));
	fplinux_ui_scrim_style(ui, scrim->object);
	/* Insert beneath this popup, above every earlier modal. */
	lv_obj_move_to_index(scrim->object, lv_obj_get_index(popup));
	lv_obj_add_event_cb(scrim->object, scrim_delete_cb, LV_EVENT_DELETE,
			    scrim);
	lv_obj_add_event_cb(popup, modal_delete_cb, LV_EVENT_DELETE, scrim);
	return true;
}

/* The modal legend bar */

static int32_t font_text_width(const char *text, const lv_font_t *font)
{
	lv_point_t size;

	lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	return size.x;
}

static int measure_legend(const char *text, void *context)
{
	const lv_font_t *font = *(const lv_font_t **)context;

	return font_text_width(text, font);
}

static void legend_bar_deleted(lv_event_t *event)
{
	struct fplinux_ui *ui = lv_event_get_user_data(event);

	ui->legend_bar = NULL;
}

/* Bar fill, a divider on top and legends in bar_text. */
lv_obj_t *fplinux_ui_softkey_bar_create(struct fplinux_ui *ui, lv_obj_t *parent)
{
	static const lv_text_align_t aligns[3] = {
		LV_TEXT_ALIGN_LEFT,
		LV_TEXT_ALIGN_CENTER,
		LV_TEXT_ALIGN_RIGHT,
	};
	const lv_font_t *font = ui->theme.font_softkey;
	lv_obj_t *bar;

	if (!parent) {
		errno = EINVAL;
		return NULL;
	}
	bar = lv_obj_create(parent);
	if (!bar) {
		errno = ENOMEM;
		return NULL;
	}
	lv_obj_remove_style_all(bar);
	lv_obj_set_scrollable(bar, false);
	lv_obj_set_size(bar, lv_pct(100), ui->metrics.softkey_height);
	lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_obj_set_style_bg_color(bar, ui->theme.bar, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(bar, ui->theme.border, 0);
	lv_obj_set_style_border_width(bar, ui->spec->divider_width, 0);
	lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
	/* Centre legends in the full bar, including its top divider. */
	lv_obj_set_style_pad_top(bar, -ui->spec->divider_width, 0);
	for (int i = 0; i < 3; ++i) {
		lv_obj_t *label = lv_label_create(bar);

		if (!label) {
			lv_obj_delete(bar);
			errno = ENOMEM;
			return NULL;
		}
		lv_obj_set_style_text_font(label, font, 0);
		lv_obj_set_style_text_color(label, ui->theme.bar_text, 0);
		lv_obj_set_style_text_align(label, aligns[i], 0);
		lv_obj_set_height(label, lv_font_get_line_height(font));
		/* Legends never move; softkey_bar_set() shortens them. */
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
		lv_label_set_text(label, "");
	}
	return bar;
}

bool fplinux_ui_softkey_bar_set(struct fplinux_ui *ui, lv_obj_t *bar,
				const char *left, const char *centre,
				const char *right)
{
	const char *legends[3] = {
		left ? left : "",
		centre ? centre : "",
		right ? right : "",
	};
	const lv_font_t *font = ui->theme.font_softkey;
	struct fplinux_softkey_box boxes[3];
	int widths[3];
	int height = ui->metrics.softkey_height;

	for (int i = 0; i < 3; ++i) {
		if (strlen(legends[i]) >= FPLINUX_UI_SESSION_TEXT_BYTES) {
			errno = ENAMETOOLONG;
			return false;
		}
		if (has_control_character(legends[i])) {
			errno = EINVAL;
			return false;
		}
		widths[i] = font_text_width(legends[i], font);
	}
	lv_obj_update_layout(bar);
	fplinux_softkey_layout(lv_obj_get_width(bar), ui->spec->legend_margin,
			       ui->spec->legend_gap, widths, boxes);
	for (int i = 0; i < 3; ++i) {
		char text[FPLINUX_UI_SESSION_TEXT_BYTES + 4];
		lv_obj_t *label = lv_obj_get_child(bar, i);
		const char *shown;

		fplinux_softkey_fit_text(legends[i], boxes[i].width,
					 measure_legend, &font, text,
					 sizeof(text));
		lv_obj_set_pos(label, boxes[i].x,
			       (height - lv_font_get_line_height(font)) / 2);
		lv_obj_set_width(label, boxes[i].width);
		shown = boxes[i].width ? text : "";
		if (strcmp(lv_label_get_text(label), shown))
			lv_label_set_text(label, shown);
	}
	return true;
}

/* A fullscreen modal shows legends above every layer, as does the launcher. */
static void show_legend_bar(struct fplinux_ui *ui)
{
	if (!ui->display ||
	    (!ui->fullscreen &&
	     ui->config_mode != FPLINUX_UI_SESSION_LAUNCHER) ||
	    !ui->modals) {
		if (ui->legend_bar)
			lv_obj_set_hidden(ui->legend_bar, true);
		return;
	}
	if (!ui->legend_bar) {
		ui->legend_bar = fplinux_ui_softkey_bar_create(
			ui, lv_display_get_layer_sys(ui->display));
		if (!ui->legend_bar)
			return;
		lv_obj_add_event_cb(ui->legend_bar, legend_bar_deleted,
				    LV_EVENT_DELETE, ui);
	}
	fplinux_ui_softkey_bar_set(ui, ui->legend_bar, ui->legends[0],
				   ui->legends[1], ui->legends[2]);
	lv_obj_set_hidden(ui->legend_bar, false);
}

void fplinux_ui_modal_set(struct fplinux_ui *ui, enum fplinux_ui_modal source,
			  bool up)
{
	unsigned int modals = up ? ui->modals | source :
				   ui->modals & ~(unsigned int)source;

	if (modals == ui->modals)
		return;
	ui->modals = modals;
	show_legend_bar(ui);
}

int fplinux_ui_modal_height(const struct fplinux_ui *ui)
{
	return ui->fullscreen ||
			       ui->config_mode == FPLINUX_UI_SESSION_LAUNCHER ?
		       ui->height - ui->metrics.softkey_height :
		       ui->height;
}

/* The system legends: nothing on the left, Select on OK, Back on the right. */
bool fplinux_ui_menu_open(struct fplinux_ui *ui,
			  enum fplinux_ui_menu_origin origin, const char *title,
			  const struct fplinux_ui_menu_item *items,
			  size_t count, fplinux_ui_menu_cb callback,
			  void *context)
{
	int error;

	if ((unsigned int)origin > FPLINUX_UI_MENU_ORIGIN_RIGHT) {
		errno = EINVAL;
		return false;
	}
	if (ui->menu) {
		errno = EBUSY;
		return false;
	}
	ui->menu = fplinux_ui_menu_view_create(ui, origin, title, items, count);
	if (!ui->menu)
		return false;
	fplinux_ui_cancel_key_repeat(ui, NULL);
	ui->menu_callback = callback;
	ui->menu_context = context;
	fplinux_ui_modal_set(ui, FPLINUX_UI_MODAL_MENU, true);
	if (send_softkeys(ui, "", word(FPLINUX_UI_LABEL_SELECT),
			  word(FPLINUX_UI_LABEL_BACK)))
		return true;
	error = errno;
	fplinux_ui_menu_view_delete(ui->menu);
	ui->menu = NULL;
	fplinux_ui_modal_set(ui, FPLINUX_UI_MODAL_MENU, false);
	fplinux_ui_screens_reclaim(ui->screens);
	errno = error;
	return false;
}

bool fplinux_ui_menu_is_open(const struct fplinux_ui *ui)
{
	return ui->menu != NULL;
}

void fplinux_ui_menu_set_position(
	struct fplinux_ui *ui, const struct fplinux_ui_menu_position *position)
{
	if (ui->menu)
		fplinux_ui_menu_view_set_position(ui->menu, position);
}

/* A failed legend update surfaces as a session error in the main loop. */
void fplinux_ui_menu_close(struct fplinux_ui *ui)
{
	if (!ui->menu)
		return;
	fplinux_ui_cancel_key_repeat(ui, NULL);
	fplinux_ui_menu_view_delete(ui->menu);
	ui->menu = NULL;
	ui->menu_callback = NULL;
	ui->menu_context = NULL;
	fplinux_ui_modal_set(ui, FPLINUX_UI_MODAL_MENU, false);
	fplinux_ui_screens_reclaim(ui->screens);
	send_softkeys(ui, ui->softkeys[0], ui->softkeys[1], ui->softkeys[2]);
}

struct fplinux_ui_screens **fplinux_ui_screens_slot(struct fplinux_ui *ui)
{
	return &ui->screens;
}
