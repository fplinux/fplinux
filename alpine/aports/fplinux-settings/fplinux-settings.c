/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-bluez-adapter.h"
#include "fplinux-language.h"
#include "fplinux-multitap.h"
#include "fplinux-system-info.h"
#include "settings-core.h"
#include "settings-bluetooth.h"
#include "settings-wifi.h"
#include "fplinux-cli.h"

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-locale.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-session.h>
#include <fplinux/fplinux-ui-system.h>
#include <libintl.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#define APP_ID "org.fplinux.settings"
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define LANGUAGE_COUNT_MAX 16
#define _(text) gettext(text)
/* Marks text for translation where it is stored; _() translates on use. */
#define N_(text) text
/*
 * As N_() with a context, for an English word that needs another Russian
 * form under another noun; translate() looks it up (musl has no pgettext).
 */
#define NC_(context, text) context "\004" text

enum command {
	COMMAND_BACK,
	COMMAND_SAVE,
	COMMAND_CANCEL,
	COMMAND_LEAVE,
	COMMAND_CLOSE,
	COMMAND_CHANGE,
};

/* In this order; the two radio rows follow their own availability. */
enum root_row {
	ROOT_DISPLAY,
	ROOT_SOUND,
	ROOT_BLUETOOTH,
	ROOT_WIFI,
	ROOT_TIME_LANGUAGE,
	ROOT_INPUT,
	ROOT_ABOUT,
	ROOT_COUNT,
};

enum sound_row {
	SOUND_MEDIA,
	SOUND_ALARM,
	SOUND_OUTPUT,
};

enum sound_class {
	SOUND_CLASS_MEDIA,
	SOUND_CLASS_ALARM,
	SOUND_CLASSES,
};

enum display_row {
	DISPLAY_BRIGHTNESS,
	DISPLAY_SCREEN_OFF,
	DISPLAY_AUTO_LOCK,
	DISPLAY_THEME,
};

enum time_language_row {
	TIME_LANGUAGE_DATE_TIME,
	TIME_LANGUAGE_LANGUAGE,
};

enum leave_destination {
	LEAVE_BACK,
	LEAVE_ROOT,
	LEAVE_CLOSE,
};

/* A choice list: its title, the SETTING name, values and their labels. */
struct choice {
	const char *title;
	const char *setting;
	const char *const *values;
	const char *const *labels;
	size_t count;
};

static const char *const screen_off_labels[] = {
	N_("15 s"), N_("30 s"), N_("1 min"), N_("2 min"), N_("5 min"),
};
static const char *const auto_lock_labels[] = {
	NC_("auto-lock", "Off"),
	N_("Immediately"),
	N_("5 s"),
	N_("30 s"),
	N_("1 min"),
	N_("5 min"),
};

_Static_assert(ARRAY_SIZE(screen_off_labels) ==
		       FPLINUX_SETTINGS_SCREEN_OFF_COUNT,
	       "each screen timeout choice has a label");
_Static_assert(ARRAY_SIZE(auto_lock_labels) == FPLINUX_SETTINGS_AUTO_LOCK_COUNT,
	       "each auto-lock choice has a label");

/* Indexed by enum fplinux_ui_theme_kind. */
static const char *const theme_values[] = {
	[FPLINUX_UI_THEME_LIGHT] = "light",
	[FPLINUX_UI_THEME_DARK] = "dark",
};
static const char *const theme_labels[] = {
	[FPLINUX_UI_THEME_LIGHT] = NC_("theme", "Light"),
	[FPLINUX_UI_THEME_DARK] = NC_("theme", "Dark"),
};

static const struct choice screen_off_choice = {
	N_("Screen timeout"),
	"screen_off_seconds",
	fplinux_settings_screen_off_values,
	screen_off_labels,
	FPLINUX_SETTINGS_SCREEN_OFF_COUNT,
};
static const struct choice auto_lock_choice = {
	N_("Auto-lock"),
	"auto_lock_seconds",
	fplinux_settings_auto_lock_values,
	auto_lock_labels,
	FPLINUX_SETTINGS_AUTO_LOCK_COUNT,
};
static const struct choice theme_choice = {
	N_("Theme"),
	"theme",
	theme_values,
	theme_labels,
	ARRAY_SIZE(theme_values),
};

enum notice {
	NOTICE_UNAVAILABLE,
	NOTICE_NOT_SAVED,
	NOTICE_FAILED,
	NOTICE_TIME_FAILED,
};

static const char *const notice_titles[] = {
	[NOTICE_UNAVAILABLE] = N_("Unavailable"),
	[NOTICE_NOT_SAVED] = N_("Not saved"),
	[NOTICE_FAILED] = N_("Not changed"),
	[NOTICE_TIME_FAILED] = N_("Date and time"),
};
static const char *const notice_texts[] = {
	[NOTICE_UNAVAILABLE] =
		N_("This setting is unavailable now. Try again later."),
	[NOTICE_NOT_SAVED] = N_(
		"The setting is applied for this session. It was not saved."),
	[NOTICE_FAILED] = N_("The setting was not changed. Try again."),
	[NOTICE_TIME_FAILED] = N_("Check the date and time. Try again."),
};

struct settings_app {
	struct fplinux_ui *ui;
	/* Screens in the stack that the application updates; NULL if not. */
	struct fplinux_ui_screen *root;
	struct fplinux_ui_screen *about;
	struct fplinux_ui_screen *display;
	struct fplinux_ui_screen *sound;
	struct fplinux_ui_screen *time_language;
	struct fplinux_ui_screen *input;
	struct fplinux_ui_screen *choice_list;
	struct fplinux_ui_screen *brightness_form;
	struct fplinux_ui_screen *volume_form;
	struct fplinux_ui_screen *date_form;
	struct fplinux_ui_screen *leave_alert;
	struct fplinux_ui_screen *change_alert;
	struct fplinux_ui_screen *notice_screen;
	struct fplinux_ui_screen *date_warning;
	enum leave_destination leave_destination;
	bool root_requested;
	bool notice_open;
	enum notice notice;
	struct fplinux_storage_result notice_storage;
	char notice_setting[FPLINUX_UI_SESSION_NAME_BYTES];

	/* Values reported by the manager; 0 or the list size when unknown. */
	unsigned int brightness;
	bool brightness_unavailable;
	unsigned int volume[SOUND_CLASSES];
	bool volume_known[SOUND_CLASSES];
	enum sound_class volume_class;
	size_t volume_gauge;
	bool volume_previewing;
	bool volume_pending;
	struct choice output_choice;
	const char *output_values[3];
	const char *output_labels[3];
	size_t output;
	char output_value[16];
	size_t screen_off;
	size_t auto_lock;

	/* The open choice list; a NULL choice is a language list. */
	const struct choice *choice;
	bool choice_pending;
	bool choice_requery;
	bool unsaved;
	char requested_setting[FPLINUX_UI_SESSION_NAME_BYTES];
	char requested_value[FPLINUX_UI_SESSION_TEXT_BYTES];
	bool input_language;
	bool input_unavailable;
	/* The theme the screens were built in; a new one rebuilds them. */
	enum fplinux_ui_theme_kind theme;
	char locale[FPLINUX_LANGUAGE_CODE_BYTES];
	char languages[LANGUAGE_COUNT_MAX][FPLINUX_LANGUAGE_CODE_BYTES];
	size_t language_count;

	/*
	 * Brightness: Left and Right preview a level that the manager does not
	 * store; it restores the stored level when the preview ends, also when
	 * this application exits or crashes. Save stores the level.
	 */
	size_t gauge_item;
	bool previewing;

	struct fplinux_bluez_adapter *bluetooth;
	struct fplinux_settings_bluetooth *bluetooth_view;
	bool bluetooth_row;
	struct fplinux_settings_wifi *wifi;
	bool wifi_row;

	/* Date and time: an item follows the clock until the user edits it. */
	size_t date_item;
	size_t time_item;
	size_t date_save_item;
	size_t current_time_item;
	bool date_show_current;
	bool date_edited;
	bool time_edited;
	bool date_pending;
	uint64_t date_revision;
	uint64_t sent_date_revision;
	char sent_time[FPLINUX_SETTINGS_TIME_BYTES];
	/* Updates the form or the Date and time row at each new minute. */
	lv_timer_t *clock_timer;
};

static void show_notice(struct settings_app *app);
static void return_root(struct settings_app *app);
static void show_sound(struct settings_app *app);

static const char *volume_setting_name(enum sound_class class, bool preview)
{
	if (class == SOUND_CLASS_ALARM)
		return preview ? "alarm_volume_preview" : "alarm_volume";
	return preview ? "volume_preview" : "volume";
}

static void end_volume_preview(struct settings_app *app)
{
	if (!app->volume_previewing)
		return;
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SETTING,
	};

	snprintf(message.name, sizeof(message.name), "%s",
		 volume_setting_name(app->volume_class, true));
	fplinux_ui_send(app->ui, &message);
	app->volume_previewing = false;
}

static void add_back_command(struct settings_app *app,
			     struct fplinux_ui_screen *screen)
{
	const struct fplinux_ui_command back = {
		.id = COMMAND_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
		.icon = fplinux_ui_glyph(app->ui, "back", FPLINUX_UI_GLYPH_ROW),
	};

	fplinux_ui_screen_add_command(screen, &back);
}

static bool send_setting(struct settings_app *app, const char *name,
			 const char *value)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SETTING,
	};

	snprintf(message.name, sizeof(message.name), "%s", name);
	snprintf(message.value, sizeof(message.value), "%s", value);
	if (!fplinux_ui_send(app->ui, &message)) {
		perror("fplinux-settings: send SETTING");
		return false;
	}
	return true;
}

/* An empty value asks the manager for the current one. */
static void query_setting(struct settings_app *app, const char *name)
{
	send_setting(app, name, "");
}

/* "brightness" stores the level; "brightness_preview" only shows it. */
static void send_level(struct settings_app *app, const char *name,
		       unsigned int level)
{
	char value[4];

	snprintf(value, sizeof(value), "%u", level);
	send_setting(app, name, value);
}

/* An empty preview value restores the stored level. */
static void end_preview(struct settings_app *app)
{
	if (!app->previewing)
		return;
	send_setting(app, "brightness_preview", "");
	app->previewing = false;
}

/* gettext() for N_() and NC_() keys: a missing translation shows the text. */
static const char *translate(const char *key)
{
	const char *text = gettext(key);
	const char *separator;

	if (text != key)
		return text;
	separator = strchr(key, '\004');
	return separator ? separator + 1 : key;
}

static const char *choice_label(const struct choice *choice, size_t index)
{
	return index < choice->count ? translate(choice->labels[index]) : "";
}

static enum fplinux_ui_theme_kind theme_kind(const struct settings_app *app)
{
	return fplinux_ui_theme(app->ui)->kind;
}

/* The value a choice list marks: the index of the manager's value. */
static size_t choice_current(const struct settings_app *app,
			     const struct choice *choice)
{
	if (choice == &screen_off_choice)
		return app->screen_off;
	if (choice == &auto_lock_choice)
		return app->auto_lock;
	if (choice == &app->output_choice)
		return app->output;
	return (size_t)theme_kind(app);
}

/* A system glyph at the left of a row; rows go without when it is missing. */
static void set_glyph(struct settings_app *app, struct fplinux_ui_screen *list,
		      size_t index, const char *name)
{
	const void *glyph =
		fplinux_ui_glyph(app->ui, name, FPLINUX_UI_GLYPH_ROW);

	if (glyph)
		fplinux_ui_list_set_icon(list, index, glyph,
					 FPLINUX_UI_LIST_ICON_IMAGE);
}

/* The current system language by its own name, as the language list shows. */
static void language_name(const char *code, char *name, size_t size)
{
	struct fplinux_language language;

	if (fplinux_language_load(FPLINUX_LANGUAGE_DIRECTORY, code, &language))
		snprintf(name, size, "%s", language.name);
	else
		snprintf(name, size, "%s", code);
}

static void show_clock(struct settings_app *app);
static void schedule_clock(struct settings_app *app);

/*
 * Pops the top screen, which the runtime then deletes, and forgets it. The
 * screen below shows the current time at once if it shows the clock.
 */
static void close_screen(struct settings_app *app,
			 struct fplinux_ui_screen **screen)
{
	if (screen == &app->choice_list)
		app->choice_pending = false;
	*screen = NULL;
	fplinux_ui_pop(app->ui);
	show_clock(app);
	schedule_clock(app);
}

static struct fplinux_ui_screen *new_list(struct settings_app *app,
					  const char *title,
					  enum fplinux_ui_list_kind kind,
					  fplinux_ui_command_cb command,
					  fplinux_ui_change_cb change)
{
	struct fplinux_ui_screen *list =
		fplinux_ui_list_new(app->ui, title, kind);

	if (!list) {
		perror("fplinux-settings: list");
		return NULL;
	}
	/* OK on a menu row opens it; the system names that "Select". */
	if (kind == FPLINUX_UI_LIST_IMPLICIT)
		fplinux_ui_list_set_select_label(list, _("Open"));
	add_back_command(app, list);
	fplinux_ui_screen_set_listener(list, command, change, app);
	return list;
}

static struct fplinux_ui_screen *new_form(struct settings_app *app,
					  const char *title)
{
	struct fplinux_ui_screen *form = fplinux_ui_form_new(app->ui, title);

	if (!form)
		perror("fplinux-settings: form");
	return form;
}

/* Choice lists: the radio marker shows the current value. */

static void show_choice_state(struct settings_app *app)
{
	size_t count = app->choice ? app->choice->count : app->language_count;
	size_t current = app->choice ? choice_current(app, app->choice) : count;
	const char *locale = app->input_language ?
				     fplinux_ui_input_locale(app->ui) :
				     fplinux_ui_locale(app->ui);

	if (!app->choice_list)
		return;
	if (!app->choice)
		for (size_t i = 0; i < count; ++i)
			if (!strcmp(app->languages[i], locale))
				current = i;
	for (size_t i = 0; i < count; ++i)
		fplinux_ui_list_set_selected(app->choice_list, i, i == current);
	fplinux_ui_list_set_select_label(app->choice_list,
					 app->choice_pending ? "" : NULL);
	fplinux_ui_screen_set_header_status(
		app->choice_list, app->choice_pending ? _("Applying…") : NULL,
		NULL);
	if (app->choice_pending)
		fplinux_ui_screen_remove_command(app->choice_list,
						 COMMAND_BACK);
	else
		add_back_command(app, app->choice_list);
}

static void change_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *alert, int command_id,
			   void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)alert;
	app->change_alert = NULL;
	if (command_id == COMMAND_CHANGE) {
		app->choice_pending = true;
		if (!send_setting(app, app->requested_setting,
				  app->requested_value))
			app->choice_pending = false;
	}
	show_choice_state(app);
	if (command_id != COMMAND_CHANGE && app->choice_list) {
		size_t count = app->choice ? app->choice->count :
					     app->language_count;

		for (size_t i = 0; i < count; ++i)
			if (fplinux_ui_list_is_selected(app->choice_list, i))
				fplinux_ui_list_set_focused(app->choice_list,
							    i);
	}
}

static void confirm_change(struct settings_app *app, const char *name,
			   const char *value, const char *title)
{
	const struct fplinux_ui_command change = {
		.id = COMMAND_CHANGE,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Change"),
		.direct = true,
		.icon = fplinux_ui_glyph(app->ui, "edit", FPLINUX_UI_GLYPH_ROW),
	};
	const struct fplinux_ui_command cancel = {
		.id = COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.label = NULL,
		.icon = fplinux_ui_glyph(app->ui, "cancel",
					 FPLINUX_UI_GLYPH_ROW),
	};
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_CONFIRMATION, title,
		_("Changing this setting closes other apps. Unsaved changes may be lost."),
		0);

	if (!alert)
		return;
	app->change_alert = alert;
	snprintf(app->requested_setting, sizeof(app->requested_setting), "%s",
		 name);
	snprintf(app->requested_value, sizeof(app->requested_value), "%s",
		 value);
	fplinux_ui_screen_add_command(alert, &change);
	fplinux_ui_screen_add_command(alert, &cancel);
	fplinux_ui_screen_set_listener(alert, change_command, NULL, app);
	fplinux_ui_push(app->ui, alert);
}

static void choice_changed(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, size_t index,
			   void *context)
{
	struct settings_app *app = context;
	const struct choice *choice = app->choice;
	size_t current = choice_current(app, choice);

	(void)ui;
	(void)list;
	show_choice_state(app);
	if (app->choice_pending)
		return;
	/*
	 * A new theme keeps this screen until the reply rebuilds it, so the
	 * parent is never shown briefly in the previous theme.
	 */
	if (index != current) {
		if (choice == &theme_choice) {
			confirm_change(app, choice->setting,
				       choice->values[index], _("Theme"));
			return;
		}
		send_setting(app, choice->setting, choice->values[index]);
	} else if (app->unsaved) {
		send_setting(app, choice->setting, choice->values[index]);
	}
	close_screen(app, &app->choice_list);
}

static void choice_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, int command_id,
			   void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)list;
	/* The reply finishes a pending change in its new theme or language. */
	if (command_id == COMMAND_BACK && !app->choice_pending)
		close_screen(app, &app->choice_list);
}

static void open_choice(struct settings_app *app, const struct choice *choice,
			size_t current)
{
	struct fplinux_ui_screen *list =
		new_list(app, _(choice->title), FPLINUX_UI_LIST_EXCLUSIVE,
			 choice_command, choice_changed);

	if (!list)
		return;
	for (size_t i = 0; i < choice->count; i++)
		fplinux_ui_list_append(list, choice_label(choice, i), NULL);
	if (current < choice->count) {
		fplinux_ui_list_set_selected(list, current, true);
		fplinux_ui_list_set_focused(list, current);
	}
	app->choice = choice;
	app->choice_list = list;
	fplinux_ui_push(app->ui, list);
}

/* The language list: installed system languages, each in its own name. */

static void language_changed(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *list, size_t index,
			     void *context)
{
	struct settings_app *app = context;
	const char *current = app->input_language ?
				      fplinux_ui_input_locale(app->ui) :
				      fplinux_ui_locale(app->ui);

	(void)ui;
	(void)list;
	show_choice_state(app);
	if (app->choice_pending)
		return;
	/* The reply switches the language; see apply_setting(). */
	if (index < app->language_count &&
	    strcmp(app->languages[index], current)) {
		if (!app->input_language) {
			confirm_change(app, "locale", app->languages[index],
				       _("Language"));
			return;
		}
		send_setting(app, "input_locale", app->languages[index]);
	} else if (index < app->language_count && app->unsaved) {
		send_setting(app,
			     app->input_language ? "input_locale" : "locale",
			     app->languages[index]);
	}
	close_screen(app, &app->choice_list);
}

static int compare_language_codes(const void *left, const void *right)
{
	return strcmp(left, right);
}

static void open_language(struct settings_app *app, bool input)
{
	char codes[LANGUAGE_COUNT_MAX][FPLINUX_LANGUAGE_CODE_BYTES];
	size_t count = fplinux_language_list(FPLINUX_LANGUAGE_DIRECTORY, codes,
					     LANGUAGE_COUNT_MAX);
	struct fplinux_language language;
	struct fplinux_multitap_languages input_languages;
	const char *current = input ? fplinux_ui_input_locale(app->ui) :
				      fplinux_ui_locale(app->ui);
	struct fplinux_ui_screen *list = new_list(
		app, input ? _("Input language") : _("Language"),
		FPLINUX_UI_LIST_EXCLUSIVE, choice_command, language_changed);

	if (!list)
		return;
	if (input) {
		fplinux_multitap_load_languages(
			&input_languages, FPLINUX_LANGUAGE_DIRECTORY, current);
		count = input_languages.count;
		for (size_t i = 0; i < count; i++)
			memcpy(codes[i], input_languages.language[i].code,
			       sizeof(codes[i]));
		qsort(codes, count, sizeof(codes[0]), compare_language_codes);
	}
	if (count > LANGUAGE_COUNT_MAX)
		count = LANGUAGE_COUNT_MAX;
	app->language_count = 0;
	for (size_t i = 0; i < count; i++) {
		size_t row = app->language_count;

		if (!fplinux_language_load(FPLINUX_LANGUAGE_DIRECTORY, codes[i],
					   &language) ||
		    !(input ? language.input : language.system))
			continue;
		memcpy(app->languages[row], codes[i], sizeof(codes[i]));
		fplinux_ui_list_append(list, language.name, NULL);
		if (!strcmp(codes[i], current)) {
			fplinux_ui_list_set_selected(list, row, true);
			fplinux_ui_list_set_focused(list, row);
		}
		app->language_count++;
	}
	app->choice = NULL;
	app->input_language = input;
	app->choice_list = list;
	fplinux_ui_push(app->ui, list);
}

/* Brightness: Left and Right move the gauge and preview the level. */

static void brightness_changed(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *form, size_t index,
			       void *context)
{
	struct settings_app *app = context;
	int level = fplinux_ui_form_get_value(form, index);

	(void)ui;
	if (index == app->gauge_item && level > 0) {
		send_level(app, "brightness_preview", (unsigned int)level);
		app->previewing = true;
	}
}

static void brightness_command(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *form, int command_id,
			       void *context)
{
	struct settings_app *app = context;
	int level = fplinux_ui_form_get_value(form, app->gauge_item);

	(void)ui;
	if (command_id == COMMAND_SAVE) {
		/* Storing the level also ends the preview. */
		if (level > 0)
			send_level(app, "brightness", (unsigned int)level);
		app->previewing = false;
	} else if (command_id == COMMAND_CANCEL) {
		end_preview(app);
	} else {
		return;
	}
	close_screen(app, &app->brightness_form);
}

static void open_brightness(struct settings_app *app)
{
	const struct fplinux_ui_command save = {
		.id = COMMAND_SAVE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Save"),
		.priority = 0,
		.icon = fplinux_ui_glyph(app->ui, "save", FPLINUX_UI_GLYPH_ROW),
	};
	const struct fplinux_ui_command cancel = {
		.id = COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.label = NULL,
		.priority = 0,
		.icon = fplinux_ui_glyph(app->ui, "cancel",
					 FPLINUX_UI_GLYPH_ROW),
	};
	struct fplinux_ui_screen *form = new_form(app, _("Brightness"));

	if (!form)
		return;
	app->gauge_item = fplinux_ui_form_append_gauge(
		form, _("Level"), FPLINUX_SETTINGS_BRIGHTNESS_MIN,
		FPLINUX_SETTINGS_BRIGHTNESS_MAX, (int)app->brightness, true);
	fplinux_ui_form_append_string(
		form, NULL, _("Left/Right: preview. Save keeps the level."));
	fplinux_ui_screen_add_command(form, &save);
	fplinux_ui_screen_add_command(form, &cancel);
	fplinux_ui_screen_set_listener(form, brightness_command,
				       brightness_changed, app);
	app->previewing = false;
	app->brightness_form = form;
	fplinux_ui_push(app->ui, form);
}

static void volume_changed(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *form, size_t index,
			   void *context)
{
	struct settings_app *app = context;
	int level = fplinux_ui_form_get_value(form, index);

	(void)ui;
	if (index != app->volume_gauge || level < 0 || app->volume_pending)
		return;
	send_level(app, volume_setting_name(app->volume_class, true),
		   (unsigned int)level);
	app->volume_previewing = true;
}

static void volume_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *form, int command_id,
			   void *context)
{
	struct settings_app *app = context;

	(void)ui;
	if (app->volume_pending)
		return;
	if (command_id == COMMAND_SAVE) {
		/* Commit the manager's latest level, including physical key steps. */
		app->volume_pending =
			send_setting(app,
				     app->volume_class == SOUND_CLASS_MEDIA ?
					     "volume_commit" :
					     "alarm_volume_commit",
				     "");
		if (app->volume_pending)
			fplinux_ui_screen_set_header_status(form, _("Saving…"),
							    NULL);
	} else if (command_id == COMMAND_CANCEL) {
		end_volume_preview(app);
		close_screen(app, &app->volume_form);
	}
}

static void open_volume(struct settings_app *app, enum sound_class class)
{
	const struct fplinux_ui_command save = {
		.id = COMMAND_SAVE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Save"),
		.icon = fplinux_ui_glyph(app->ui, "save", FPLINUX_UI_GLYPH_ROW),
	};
	const struct fplinux_ui_command cancel = {
		.id = COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.icon = fplinux_ui_glyph(app->ui, "cancel",
					 FPLINUX_UI_GLYPH_ROW),
	};
	struct fplinux_ui_screen *form =
		new_form(app, class == SOUND_CLASS_MEDIA ? _("Media volume") :
							   _("Alarm volume"));

	if (!form)
		return;
	app->volume_class = class;
	app->volume_gauge = fplinux_ui_form_append_gauge(
		form, _("Level"), 0, 10, (int)app->volume[class], true);
	fplinux_ui_form_append_string(
		form, NULL, _("Left/Right: volume. Save keeps the level."));
	fplinux_ui_screen_add_command(form, &save);
	fplinux_ui_screen_add_command(form, &cancel);
	fplinux_ui_screen_set_listener(form, volume_command, volume_changed,
				       app);
	app->volume_form = form;
	app->volume_previewing = false;
	fplinux_ui_push(app->ui, form);
}

/* Date and time: OK edits an item; Save sets the clock through the manager. */

/*
 * The Date and time row of Time & language shows the date and time as its
 * detail, as the Language row shows the language.
 */
static void show_date_time_row(struct settings_app *app, time_t now)
{
	char date[32];
	char clock[16];
	char detail[48];

	if (!app->time_language)
		return;
	if (!fplinux_ui_format_date(app->ui, now, FPLINUX_UI_DATE_SHORT, date,
				    sizeof(date)))
		date[0] = '\0';
	if (!fplinux_ui_format_date(app->ui, now, FPLINUX_UI_DATE_CLOCK, clock,
				    sizeof(clock)))
		clock[0] = '\0';
	if (!fplinux_settings_date_time_detail(date, clock, detail,
					       sizeof(detail)))
		detail[0] = '\0';
	fplinux_ui_list_set(app->time_language, TIME_LANGUAGE_DATE_TIME,
			    _("Date and time"), detail);
}

/*
 * The Date and time row and the form items that the user has not edited show
 * the current date and time.
 */
static void show_clock(struct settings_app *app)
{
	time_t now = time(NULL);
	char date[48], clock[32], text[160];

	show_date_time_row(app, now);
	if (!app->date_form)
		return;
	if (!app->date_edited && !app->date_pending)
		fplinux_ui_form_set_date(app->date_form, app->date_item, now);
	if (!app->time_edited && !app->date_pending)
		fplinux_ui_form_set_date(app->date_form, app->time_item, now);
	if (app->date_show_current &&
	    fplinux_ui_format_date(app->ui, now, FPLINUX_UI_DATE_SHORT, date,
				   sizeof(date)) &&
	    fplinux_ui_format_date(app->ui, now, FPLINUX_UI_DATE_CLOCK, clock,
				   sizeof(clock))) {
		snprintf(text, sizeof(text), _("Current time (UTC): %s %s"),
			 date, clock);
		fplinux_ui_form_set_string(app->date_form,
					   app->current_time_item, text);
	}
}

/*
 * Whether the screen of Settings in front shows the clock: the form with an
 * unedited item, or Time & language, also under an alert, which goes away
 * without telling Settings.
 */
static bool clock_shown(const struct settings_app *app)
{
	if (app->date_form)
		return app->date_show_current || !app->date_edited ||
		       !app->time_edited;
	return app->time_language && !app->choice_list;
}

/* The screen in front follows the clock at each new minute. */
static void schedule_clock(struct settings_app *app)
{
	time_t now = time(NULL);
	const struct tm *local = localtime(&now);
	uint32_t delay_ms;

	if (!clock_shown(app) || !fplinux_ui_is_visible(app->ui) || !local) {
		lv_timer_pause(app->clock_timer);
		return;
	}
	delay_ms = (uint32_t)(60 - local->tm_sec % 60) * 1000U + 50U;
	lv_timer_set_period(app->clock_timer, delay_ms);
	lv_timer_reset(app->clock_timer);
	lv_timer_resume(app->clock_timer);
}

static void clock_tick(lv_timer_t *timer)
{
	struct settings_app *app = lv_timer_get_user_data(timer);

	show_clock(app);
	schedule_clock(app);
}

static void date_commands(struct settings_app *app)
{
	if (!app->date_form)
		return;
	fplinux_ui_screen_set_header_status(
		app->date_form, app->date_pending ? _("Saving…") : NULL, NULL);
	fplinux_ui_form_set_hidden(app->date_form, app->date_save_item,
				   app->date_pending);
	if (app->date_pending)
		fplinux_ui_screen_remove_command(app->date_form, COMMAND_BACK);
	else
		add_back_command(app, app->date_form);
}

static void date_warning_closed(struct fplinux_ui *ui,
				struct fplinux_ui_screen *alert, int command_id,
				void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)alert;
	(void)command_id;
	app->date_warning = NULL;
}

/*
 * The edited items count; the others are read from the clock now, with its
 * seconds, so saving only a new date keeps the time running. The submitted
 * values remain available until the manager confirms the change.
 */
static void save_date_time(struct settings_app *app)
{
	time_t date = fplinux_ui_form_get_date(app->date_form, app->date_item);
	time_t clock = fplinux_ui_form_get_date(app->date_form, app->time_item);
	char value[FPLINUX_SETTINGS_TIME_BYTES];
	struct fplinux_ui_screen *alert;
	time_t now = time(NULL);

	if (app->sent_time[0] && app->sent_date_revision == app->date_revision)
		snprintf(value, sizeof(value), "%s", app->sent_time);
	else if (!fplinux_settings_save_time(now,
					     app->date_edited ? &date : NULL,
					     app->time_edited ? &clock : NULL,
					     value, sizeof(value)))
		return;
	if (!fplinux_settings_time_in_range(value)) {
		alert = fplinux_ui_alert_new(
			app->ui, FPLINUX_UI_ALERT_WARNING, _("Date and time"),
			_("Year must be between 1980 and 2099."), 0);
		if (alert) {
			app->date_warning = alert;
			fplinux_ui_screen_set_listener(
				alert, date_warning_closed, NULL, app);
			fplinux_ui_push(app->ui, alert);
		}
		return;
	}
	if (!send_setting(app, "time", value))
		return;
	app->date_pending = true;
	app->sent_date_revision = app->date_revision;
	snprintf(app->sent_time, sizeof(app->sent_time), "%s", value);
	if (!app->date_edited)
		fplinux_ui_form_set_date(app->date_form, app->date_item, now);
	if (!app->time_edited)
		fplinux_ui_form_set_date(app->date_form, app->time_item, now);
	app->date_edited = true;
	app->time_edited = true;
	date_commands(app);
}

static void leave_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *alert, int command_id,
			  void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)alert;
	app->leave_alert = NULL;
	if (app->leave_destination == LEAVE_CLOSE) {
		enum fplinux_ui_close_response response =
			FPLINUX_UI_CLOSE_CANCEL;

		if (command_id == COMMAND_LEAVE)
			response =
				app->bluetooth_view ?
					fplinux_settings_bluetooth_prepare_close(
						app->bluetooth_view) :
					FPLINUX_UI_CLOSE_READY;
		if (response != FPLINUX_UI_CLOSE_WAIT)
			fplinux_ui_close_reply(app->ui, response);
	} else if (command_id == COMMAND_LEAVE) {
		if (app->leave_destination == LEAVE_ROOT)
			return_root(app);
		else
			close_screen(app, &app->date_form);
	}
	app->leave_destination = LEAVE_BACK;
}

static void confirm_leave(struct settings_app *app)
{
	const struct fplinux_ui_command leave = {
		.id = COMMAND_LEAVE,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Leave"),
		.direct = true,
		.icon = fplinux_ui_glyph(app->ui, "exit", FPLINUX_UI_GLYPH_ROW),
	};
	const struct fplinux_ui_command cancel = {
		.id = COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.label = NULL,
		.priority = 0,
		.icon = fplinux_ui_glyph(app->ui, "cancel",
					 FPLINUX_UI_GLYPH_ROW),
	};
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_CONFIRMATION, _("Date and time"),
		app->leave_destination == LEAVE_ROOT ?
			_("Return to the start without saving?") :
			_("Leave without saving?"),
		0);

	if (!alert)
		return;
	app->leave_alert = alert;
	fplinux_ui_screen_add_command(alert, &leave);
	fplinux_ui_screen_add_command(alert, &cancel);
	fplinux_ui_screen_set_listener(alert, leave_command, NULL, app);
	fplinux_ui_push(app->ui, alert);
}

static enum fplinux_ui_close_response prepare_close(struct fplinux_ui *ui,
						    void *context)
{
	struct settings_app *app = context;

	(void)ui;
	if (!app->date_form ||
	    (!app->date_edited && !app->time_edited && !app->date_pending &&
	     !fplinux_ui_form_is_editing(app->date_form)))
		return app->bluetooth_view ?
			       fplinux_settings_bluetooth_prepare_close(
				       app->bluetooth_view) :
			       FPLINUX_UI_CLOSE_READY;
	if (!app->leave_alert)
		confirm_leave(app);
	app->leave_destination = LEAVE_CLOSE;
	return app->leave_alert ? FPLINUX_UI_CLOSE_WAIT :
				  FPLINUX_UI_CLOSE_CANCEL;
}

static void date_time_changed(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *form, size_t index,
			      void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)form;
	if (index == app->date_item)
		app->date_edited = true;
	else if (index == app->time_item)
		app->time_edited = true;
	++app->date_revision;
	schedule_clock(app);
}

static void date_time_command(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *form, int command_id,
			      void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)form;
	if (app->date_pending)
		return;
	if (command_id == COMMAND_SAVE) {
		if (app->date_edited || app->time_edited) {
			save_date_time(app);
			return;
		}
		close_screen(app, &app->date_form);
	} else if (command_id == COMMAND_BACK) {
		if (app->date_edited || app->time_edited)
			confirm_leave(app);
		else
			close_screen(app, &app->date_form);
	}
}

static void open_date_time(struct settings_app *app)
{
	const struct fplinux_ui_command save = {
		.id = COMMAND_SAVE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Save"),
	};
	struct fplinux_ui_screen *form = new_form(app, _("Date and time"));
	time_t now = time(NULL);

	if (!form)
		return;
	app->date_item = fplinux_ui_form_append_date(form, _("Date"),
						     FPLINUX_UI_DATE, now);
	app->time_item = fplinux_ui_form_append_date(form, _("Time (UTC)"),
						     FPLINUX_UI_TIME, now);
	app->date_save_item =
		fplinux_ui_form_append_value(form, save.label, NULL);
	fplinux_ui_form_add_item_command(form, app->date_save_item, &save);
	app->current_time_item = fplinux_ui_form_append_string(form, NULL, "");
	app->date_show_current = false;
	fplinux_ui_screen_set_listener(form, date_time_command,
				       date_time_changed, app);
	app->date_edited = false;
	app->time_edited = false;
	app->date_pending = false;
	app->sent_time[0] = '\0';
	app->date_form = form;
	date_commands(app);
	fplinux_ui_push(app->ui, form);
	schedule_clock(app);
}

/* Menus: rows show the current value on a second line, such as "7". */

static void show_display(struct settings_app *app)
{
	const char *brightness = "";
	char level[8];

	if (!app->display)
		return;
	if (app->brightness_unavailable) {
		brightness = _("Unavailable");
	} else if (app->brightness) {
		snprintf(level, sizeof(level), "%u", app->brightness);
		brightness = level;
	}
	fplinux_ui_list_set(app->display, DISPLAY_BRIGHTNESS, _("Brightness"),
			    brightness);
	fplinux_ui_list_set(app->display, DISPLAY_SCREEN_OFF,
			    _(screen_off_choice.title),
			    choice_label(&screen_off_choice, app->screen_off));
	fplinux_ui_list_set(app->display, DISPLAY_AUTO_LOCK,
			    _(auto_lock_choice.title),
			    choice_label(&auto_lock_choice, app->auto_lock));
	fplinux_ui_list_set(app->display, DISPLAY_THEME, _(theme_choice.title),
			    choice_label(&theme_choice, theme_kind(app)));
}

static void display_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *list, int command_id,
			    void *context)
{
	struct settings_app *app = context;

	(void)ui;
	if (command_id == COMMAND_BACK) {
		close_screen(app, &app->display);
		return;
	}
	if (command_id != FPLINUX_UI_SELECT_COMMAND)
		return;
	switch (fplinux_ui_list_get_focused(list)) {
	case DISPLAY_BRIGHTNESS:
		/* The gauge starts from the level the manager reported. */
		if (app->brightness)
			open_brightness(app);
		break;
	case DISPLAY_SCREEN_OFF:
		open_choice(app, &screen_off_choice, app->screen_off);
		break;
	case DISPLAY_AUTO_LOCK:
		open_choice(app, &auto_lock_choice, app->auto_lock);
		break;
	case DISPLAY_THEME:
		open_choice(app, &theme_choice, (size_t)theme_kind(app));
		break;
	}
}

static void open_display(struct settings_app *app, size_t focus)
{
	struct fplinux_ui_screen *list = new_list(app, _("Display"),
						  FPLINUX_UI_LIST_IMPLICIT,
						  display_command, NULL);

	if (!list)
		return;
	fplinux_ui_list_set_value_details(list);
	fplinux_ui_list_append(list, _("Brightness"), NULL);
	fplinux_ui_list_append(list, _(screen_off_choice.title), NULL);
	fplinux_ui_list_append(list, _(auto_lock_choice.title), NULL);
	fplinux_ui_list_append(list, _(theme_choice.title), NULL);
	set_glyph(app, list, DISPLAY_BRIGHTNESS, "brightness");
	set_glyph(app, list, DISPLAY_SCREEN_OFF, "screen-timeout");
	set_glyph(app, list, DISPLAY_AUTO_LOCK, "auto-lock");
	set_glyph(app, list, DISPLAY_THEME, "theme");
	fplinux_ui_list_set_focused(list, focus);
	app->display = list;
	show_display(app);
	fplinux_ui_push(app->ui, list);
}

static void show_sound(struct settings_app *app)
{
	char level[2][4];

	if (!app->sound)
		return;
	for (unsigned int i = 0; i < SOUND_CLASSES; ++i)
		snprintf(level[i], sizeof(level[i]), "%u", app->volume[i]);
	fplinux_ui_list_set(app->sound, SOUND_MEDIA, _("Media volume"),
			    app->volume_known[SOUND_CLASS_MEDIA] ?
				    level[0] :
				    _("Unavailable"));
	fplinux_ui_list_set(app->sound, SOUND_ALARM, _("Alarm volume"),
			    app->volume_known[SOUND_CLASS_ALARM] ?
				    level[1] :
				    _("Unavailable"));
	fplinux_ui_list_set(app->sound, SOUND_OUTPUT, _("Output"),
			    app->output_choice.count ?
				    choice_label(&app->output_choice,
						 app->output) :
				    _("Unavailable"));
}

static void sound_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			  int command_id, void *context)
{
	struct settings_app *app = context;
	size_t focused = fplinux_ui_list_get_focused(list);

	(void)ui;
	if (command_id == COMMAND_BACK) {
		close_screen(app, &app->sound);
		return;
	}
	if (command_id != FPLINUX_UI_SELECT_COMMAND)
		return;
	if (focused == SOUND_MEDIA && app->volume_known[SOUND_CLASS_MEDIA])
		open_volume(app, SOUND_CLASS_MEDIA);
	else if (focused == SOUND_ALARM && app->volume_known[SOUND_CLASS_ALARM])
		open_volume(app, SOUND_CLASS_ALARM);
	else if (focused == SOUND_OUTPUT && app->output_choice.count)
		open_choice(app, &app->output_choice, app->output);
}

static void open_sound(struct settings_app *app)
{
	struct fplinux_ui_screen *list = new_list(
		app, _("Sound"), FPLINUX_UI_LIST_IMPLICIT, sound_command, NULL);

	if (!list)
		return;
	fplinux_ui_list_set_value_details(list);
	fplinux_ui_list_append(list, _("Media volume"), NULL);
	fplinux_ui_list_append(list, _("Alarm volume"), NULL);
	fplinux_ui_list_append(list, _("Output"), NULL);
	set_glyph(app, list, SOUND_MEDIA, "volume");
	set_glyph(app, list, SOUND_ALARM, "alarm");
	set_glyph(app, list, SOUND_OUTPUT, "volume");
	app->sound = list;
	show_sound(app);
	fplinux_ui_push(app->ui, list);
	query_setting(app, "audio_output");
	query_setting(app, "audio_outputs");
}

static void show_time_language(struct settings_app *app)
{
	char name[FPLINUX_LANGUAGE_TEXT_BYTES];

	if (!app->time_language)
		return;
	show_date_time_row(app, time(NULL));
	language_name(fplinux_ui_locale(app->ui), name, sizeof(name));
	fplinux_ui_list_set(app->time_language, TIME_LANGUAGE_LANGUAGE,
			    _("Language"), name);
}

static void time_language_command(struct fplinux_ui *ui,
				  struct fplinux_ui_screen *list,
				  int command_id, void *context)
{
	struct settings_app *app = context;

	(void)ui;
	if (command_id == COMMAND_BACK) {
		close_screen(app, &app->time_language);
		return;
	}
	if (command_id != FPLINUX_UI_SELECT_COMMAND)
		return;
	if (fplinux_ui_list_get_focused(list) == TIME_LANGUAGE_DATE_TIME)
		open_date_time(app);
	else
		open_language(app, false);
}

static void open_time_language(struct settings_app *app, size_t focus)
{
	struct fplinux_ui_screen *list = new_list(app, _("Time & language"),
						  FPLINUX_UI_LIST_IMPLICIT,
						  time_language_command, NULL);

	if (!list)
		return;
	fplinux_ui_list_set_value_details(list);
	fplinux_ui_list_append(list, _("Date and time"), NULL);
	fplinux_ui_list_append(list, _("Language"), NULL);
	set_glyph(app, list, TIME_LANGUAGE_DATE_TIME, "date-time");
	set_glyph(app, list, TIME_LANGUAGE_LANGUAGE, "language");
	fplinux_ui_list_set_focused(list, focus);
	app->time_language = list;
	show_time_language(app);
	fplinux_ui_push(app->ui, list);
	schedule_clock(app);
}

static void show_input(struct settings_app *app)
{
	char name[FPLINUX_LANGUAGE_TEXT_BYTES];

	if (!app->input)
		return;
	language_name(fplinux_ui_input_locale(app->ui), name, sizeof(name));
	fplinux_ui_list_set(app->input, 0, _("Input language"),
			    app->input_unavailable ? _("Unavailable") : name);
}

static void input_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			  int command_id, void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)list;
	if (command_id == COMMAND_BACK)
		close_screen(app, &app->input);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND)
		open_language(app, true);
}

static void open_input(struct settings_app *app)
{
	struct fplinux_ui_screen *list = new_list(
		app, _("Input"), FPLINUX_UI_LIST_IMPLICIT, input_command, NULL);

	if (!list)
		return;
	fplinux_ui_list_set_value_details(list);
	fplinux_ui_list_append(list, _("Input language"), NULL);
	set_glyph(app, list, 0, "language");
	app->input = list;
	show_input(app);
	fplinux_ui_push(app->ui, list);
}

static void about_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)form;
	if (command_id == COMMAND_BACK)
		close_screen(app, &app->about);
}

static void append_memory(struct settings_app *app,
			  struct fplinux_ui_screen *form)
{
	unsigned long available_kib;
	unsigned long total_kib;
	char available[16];
	char total[16];
	char value[48];

	if (!fplinux_settings_read_memory("/proc/meminfo", &available_kib,
					  &total_kib) ||
	    !fplinux_ui_format_number(app->ui, available_kib / 1024.0, 1,
				      available, sizeof(available)) ||
	    !fplinux_ui_format_number(app->ui, total_kib / 1024.0, 1, total,
				      sizeof(total)))
		return;
	/* A no-break space keeps the unit with its number. */
	snprintf(value, sizeof(value), _("%s / %s\u00a0MiB"), available, total);
	fplinux_ui_form_append_property(form, _("Free memory"), value);
}

static void append_logo(struct settings_app *app,
			struct fplinux_ui_screen *form)
{
	char path[128];

	if (fplinux_settings_about_logo(fplinux_ui_metrics(app->ui)->profile,
					theme_kind(app) ==
						FPLINUX_UI_THEME_DARK,
					path, sizeof(path)) &&
	    fplinux_ui_form_append_image(form, NULL, path) == SIZE_MAX)
		fprintf(stderr, "fplinux-settings: %s: %s\n", path,
			strerror(errno));
}

static void open_about(struct settings_app *app)
{
	struct fplinux_ui_screen *form = new_form(app, _("About"));
	struct utsname system;
	char value[sizeof(system.release) + 32];

	if (!form)
		return;
	append_logo(app, form);
	if (fplinux_system_info_read_os_name("/etc/os-release", value,
					     sizeof(value)))
		fplinux_ui_form_append_property(form, _("System"), value);
	if (fplinux_system_info_read_os_field("/etc/os-release", "BUILD_ID",
					      value, sizeof(value)))
		fplinux_ui_form_append_property(form, _("Build"), value);
	if (fplinux_system_info_read_line("/proc/device-tree/model", value,
					  sizeof(value)))
		fplinux_ui_form_append_property(form, _("Model"), value);
	if (fplinux_system_info_read_line("/etc/alpine-release", value,
					  sizeof(value)))
		fplinux_ui_form_append_property(form, "Alpine Linux", value);
	if (uname(&system) == 0)
		fplinux_ui_form_append_property(form, _("Kernel"),
						system.release);
	append_memory(app, form);
	add_back_command(app, form);
	fplinux_ui_screen_set_listener(form, about_command, NULL, app);
	app->about = form;
	fplinux_ui_push(app->ui, form);
}

/* Settings shares one BlueZ client with its Bluetooth controller. */

static bool bluetooth_present(const struct settings_app *app)
{
	return app->bluetooth && fplinux_bluez_adapter_present(app->bluetooth);
}

static const char *bluetooth_state(const struct settings_app *app)
{
	if (!bluetooth_present(app))
		return _("Unavailable");
	return fplinux_bluez_adapter_powered(app->bluetooth) ? _("On") :
							       _("Off");
}

static bool root_has_row(const struct settings_app *app, enum root_row row)
{
	if (row == ROOT_BLUETOOTH)
		return app->bluetooth_row;
	if (row == ROOT_WIFI)
		return app->wifi_row;
	return true;
}

static size_t root_index(const struct settings_app *app, enum root_row row)
{
	size_t index = 0;

	for (enum root_row preceding = 0; preceding < row; ++preceding)
		if (root_has_row(app, preceding))
			++index;
	return index;
}

static enum root_row root_row_at(const struct settings_app *app, size_t index)
{
	for (enum root_row row = 0; row < ROOT_COUNT; ++row)
		if (root_has_row(app, row) && index-- == 0)
			return row;
	return ROOT_COUNT;
}

/* The root row comes and goes with the adapter; the focus stays on its row. */
static void show_root_bluetooth(struct settings_app *app)
{
	bool present = bluetooth_present(app);
	size_t index = root_index(app, ROOT_BLUETOOTH);

	if (!app->root)
		return;
	if (present && !app->bluetooth_row) {
		if (fplinux_ui_list_insert(app->root, index, _("Bluetooth"),
					   NULL) == SIZE_MAX)
			return;
		set_glyph(app, app->root, index, "bluetooth");
	} else if (!present && app->bluetooth_row) {
		fplinux_ui_list_delete(app->root, index);
	}
	app->bluetooth_row = present;
	if (present)
		fplinux_ui_list_set(app->root, index, _("Bluetooth"),
				    bluetooth_state(app));
}

static const char *wifi_state(const struct settings_app *app)
{
	return fplinux_settings_wifi_power(app->wifi) == FPLINUX_UI_WIFI_ON ?
		       _("On") :
		       _("Off");
}

static void show_root_wifi(struct settings_app *app)
{
	bool present = fplinux_settings_wifi_power(app->wifi) !=
		       FPLINUX_UI_WIFI_UNAVAILABLE;
	size_t index = root_index(app, ROOT_WIFI);

	if (!app->root)
		return;
	if (present && !app->wifi_row) {
		if (fplinux_ui_list_insert(app->root, index, _("Wi-Fi"),
					   wifi_state(app)) == SIZE_MAX)
			return;
		set_glyph(app, app->root, index, "wifi");
	} else if (!present && app->wifi_row) {
		fplinux_ui_list_delete(app->root, index);
	}
	app->wifi_row = present;
	if (present)
		fplinux_ui_list_set(app->root, index, _("Wi-Fi"),
				    wifi_state(app));
}

static void receive_wifi(struct settings_app *app,
			 const struct fplinux_ui_session_message *message)
{
	if (!app->wifi && message->type == FPLINUX_UI_SESSION_WIFI_STATE &&
	    message->wifi_state.power != FPLINUX_UI_WIFI_UNAVAILABLE)
		app->wifi = fplinux_settings_wifi_new(app->ui);
	if (app->wifi)
		fplinux_settings_wifi_receive(app->wifi, message);
	show_root_wifi(app);
}

static void bluetooth_finished(bool applied, void *context)
{
	struct settings_app *app = context;

	show_root_bluetooth(app);
	if (app->bluetooth_view)
		fplinux_settings_bluetooth_finished(app->bluetooth_view,
						    applied);
}

static void bluetooth_changed(void *context)
{
	struct settings_app *app = context;

	show_root_bluetooth(app);
	if (app->bluetooth_view)
		fplinux_settings_bluetooth_changed(app->bluetooth_view);
}

static void bluetooth_ready(struct fplinux_ui *ui, int fd, short events,
			    void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)fd;
	if (events & POLLIN)
		fplinux_bluez_adapter_dispatch(app->bluetooth);
}

static void open_bluetooth(struct settings_app *app)
{
	if (!app->bluetooth_view)
		app->bluetooth_view =
			fplinux_settings_bluetooth_new(app->ui, app->bluetooth);
	if (app->bluetooth_view)
		fplinux_settings_bluetooth_show(app->bluetooth_view);
}

static void root_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			 int command_id, void *context)
{
	struct settings_app *app = context;

	if (command_id == COMMAND_BACK) {
		/* Back on the first screen leaves the application. */
		fplinux_ui_go_back(ui);
		return;
	}
	if (command_id != FPLINUX_UI_SELECT_COMMAND)
		return;
	switch (root_row_at(app, fplinux_ui_list_get_focused(list))) {
	case ROOT_BLUETOOTH:
		open_bluetooth(app);
		break;
	case ROOT_WIFI:
		fplinux_settings_wifi_show(app->wifi);
		break;
	case ROOT_DISPLAY:
		open_display(app, DISPLAY_BRIGHTNESS);
		break;
	case ROOT_SOUND:
		open_sound(app);
		break;
	case ROOT_TIME_LANGUAGE:
		open_time_language(app, TIME_LANGUAGE_DATE_TIME);
		break;
	case ROOT_INPUT:
		open_input(app);
		break;
	case ROOT_ABOUT:
		open_about(app);
		break;
	case ROOT_COUNT:
		break;
	}
}

static void show_root(struct settings_app *app)
{
	char value[FPLINUX_LANGUAGE_TEXT_BYTES];

	if (!app->root)
		return;
	fplinux_ui_list_set(app->root, root_index(app, ROOT_DISPLAY),
			    _("Display"),
			    choice_label(&theme_choice, theme_kind(app)));
	snprintf(value, sizeof(value), _("Media: %u/10"),
		 app->volume[SOUND_CLASS_MEDIA]);
	fplinux_ui_list_set(app->root, root_index(app, ROOT_SOUND), _("Sound"),
			    app->volume_known[SOUND_CLASS_MEDIA] ?
				    value :
				    _("Unavailable"));
	show_root_bluetooth(app);
	show_root_wifi(app);
	language_name(fplinux_ui_locale(app->ui), value, sizeof(value));
	fplinux_ui_list_set(app->root, root_index(app, ROOT_TIME_LANGUAGE),
			    _("Time & language"), value);
	language_name(fplinux_ui_input_locale(app->ui), value, sizeof(value));
	fplinux_ui_list_set(app->root, root_index(app, ROOT_INPUT), _("Input"),
			    app->input_unavailable ? _("Unavailable") : value);
}

static struct fplinux_ui_screen *create_root(struct settings_app *app)
{
	struct fplinux_ui_screen *list = new_list(app, _("Settings"),
						  FPLINUX_UI_LIST_IMPLICIT,
						  root_command, NULL);
	char value[128];

	if (!list)
		return NULL;
	fplinux_ui_list_set_value_details(list);
	app->bluetooth_row = bluetooth_present(app);
	app->wifi_row = fplinux_settings_wifi_power(app->wifi) !=
			FPLINUX_UI_WIFI_UNAVAILABLE;
	fplinux_ui_list_append(list, _("Display"), NULL);
	fplinux_ui_list_append(list, _("Sound"), NULL);
	if (app->bluetooth_row)
		fplinux_ui_list_append(list, _("Bluetooth"),
				       bluetooth_state(app));
	if (app->wifi_row)
		fplinux_ui_list_append(list, _("Wi-Fi"), wifi_state(app));
	fplinux_ui_list_append(list, _("Time & language"), NULL);
	fplinux_ui_list_append(list, _("Input"), NULL);
	fplinux_ui_list_append(
		list, _("About"),
		fplinux_system_info_read_os_name("/etc/os-release", value,
						 sizeof(value)) ?
			value :
			NULL);
	set_glyph(app, list, root_index(app, ROOT_DISPLAY), "display");
	set_glyph(app, list, root_index(app, ROOT_SOUND), "volume");
	if (app->bluetooth_row)
		set_glyph(app, list, root_index(app, ROOT_BLUETOOTH),
			  "bluetooth");
	if (app->wifi_row)
		set_glyph(app, list, root_index(app, ROOT_WIFI), "wifi");
	set_glyph(app, list, root_index(app, ROOT_TIME_LANGUAGE), "date-time");
	set_glyph(app, list, root_index(app, ROOT_INPUT), "keyboard");
	set_glyph(app, list, root_index(app, ROOT_ABOUT), "about");
	return list;
}

static void return_root(struct settings_app *app)
{
	struct fplinux_ui_screen **children[] = {
		&app->choice_list, &app->brightness_form, &app->date_form,
		&app->about,	   &app->display,	  &app->time_language,
		&app->input,	   &app->volume_form,	  &app->sound,
	};

	fplinux_ui_menu_close(app->ui);
	fplinux_settings_wifi_reset(app->wifi);
	if (app->bluetooth_view)
		fplinux_settings_bluetooth_reset(app->bluetooth_view);
	end_preview(app);
	end_volume_preview(app);
	for (size_t index = 0; index < ARRAY_SIZE(children); ++index)
		if (*children[index])
			close_screen(app, children[index]);
	app->choice_requery = false;
	app->root_requested = false;
}

static void open_root(struct settings_app *app)
{
	if (app->leave_alert || app->change_alert || app->notice_open ||
	    app->date_warning)
		return;
	if (app->date_pending || app->choice_pending || app->volume_pending) {
		app->root_requested = true;
		return;
	}
	app->root_requested = false;
	if (app->date_form && (app->date_edited || app->time_edited ||
			       fplinux_ui_form_is_editing(app->date_form))) {
		app->leave_destination = LEAVE_ROOT;
		confirm_leave(app);
		return;
	}
	return_root(app);
}

/* Appearance changes rebuild the current destination, including the root. */
static void rebuild(struct settings_app *app)
{
	struct fplinux_ui_screen *root;
	bool display = app->display != NULL;
	bool sound = app->sound != NULL;
	bool time_language = app->time_language != NULL;
	bool input = app->input != NULL;
	bool about = app->about != NULL;
	bool bluetooth = app->bluetooth_view &&
			 fplinux_settings_bluetooth_shown(app->bluetooth_view);
	bool wifi = fplinux_settings_wifi_shown(app->wifi);
	bool notice = app->notice_open;
	bool root_requested = app->root_requested;
	enum root_row focus =
		root_row_at(app, fplinux_ui_list_get_focused(app->root));

	if (notice)
		close_screen(app, &app->notice_screen);
	return_root(app);
	app->theme = theme_kind(app);
	snprintf(app->locale, sizeof(app->locale), "%s",
		 fplinux_ui_locale(app->ui));
	root = create_root(app);
	if (!root)
		return;
	fplinux_ui_list_set_focused(root, root_index(app, focus));
	fplinux_ui_replace(app->ui, root);
	app->root = root;
	show_root(app);
	if (!root_requested && display)
		open_display(app, DISPLAY_THEME);
	else if (!root_requested && sound)
		open_sound(app);
	else if (!root_requested && time_language)
		open_time_language(app, TIME_LANGUAGE_LANGUAGE);
	else if (!root_requested && input)
		open_input(app);
	else if (!root_requested && about)
		open_about(app);
	else if (!root_requested && bluetooth)
		open_bluetooth(app);
	else if (!root_requested && wifi)
		fplinux_settings_wifi_show(app->wifi);
	if (notice)
		show_notice(app);
}

/* Replies from the manager */

static void receive_system_levels(struct settings_app *app)
{
	struct fplinux_system_level_state state;

	if (fplinux_ui_system_level_get(
		    app->ui, FPLINUX_SYSTEM_LEVEL_BRIGHTNESS, &state)) {
		bool ready = state.availability == FPLINUX_SYSTEM_LEVEL_READY;

		app->brightness = ready ? state.value : 0;
		app->brightness_unavailable = state.availability ==
					      FPLINUX_SYSTEM_LEVEL_UNAVAILABLE;
		if (ready && app->brightness_form && !app->previewing)
			fplinux_ui_form_set_value(app->brightness_form,
						  app->gauge_item,
						  (int)state.value);
	}
	for (enum sound_class class = 0; class < SOUND_CLASSES; ++class) {
		enum fplinux_system_level_kind kind =
			class == SOUND_CLASS_MEDIA ?
				FPLINUX_SYSTEM_LEVEL_MEDIA :
				FPLINUX_SYSTEM_LEVEL_ALARM;

		if (!fplinux_ui_system_level_get(app->ui, kind, &state))
			continue;
		app->volume_known[class] = state.availability ==
					   FPLINUX_SYSTEM_LEVEL_READY;
		if (app->volume_known[class]) {
			app->volume[class] = state.value;
			if (app->volume_form && app->volume_class == class &&
			    !state.pending)
				fplinux_ui_form_set_value(app->volume_form,
							  app->volume_gauge,
							  (int)state.value);
		}
	}
	show_display(app);
	show_sound(app);
	show_root(app);
}

static void apply_setting(struct settings_app *app, const char *name,
			  const char *value)
{
	if (!strcmp(name, "volume") || !strcmp(name, "alarm_volume") ||
	    !strcmp(name, "volume_preview") ||
	    !strcmp(name, "alarm_volume_preview")) {
		enum sound_class class = !strncmp(name, "alarm_", 6) ?
						 SOUND_CLASS_ALARM :
						 SOUND_CLASS_MEDIA;
		char *end;
		unsigned long level = strtoul(value, &end, 10);

		if (!value[0] || *end || level > 10)
			return;
		if (app->volume_form && app->volume_class == class &&
		    app->volume_previewing)
			fplinux_ui_form_set_value(app->volume_form,
						  app->volume_gauge,
						  (int)level);
		return;
	} else if (!strcmp(name, "audio_outputs")) {
		static const char *const values[] = { "AUTO", "SPEAKER",
						      "HEADPHONES" };
		static const char *const labels[] = { N_("Automatic"),
						      N_("Speaker"),
						      N_("Headphones") };
		const char *cursor = value;

		app->output_choice = (struct choice){
			.title = N_("Output"),
			.setting = "audio_output",
			.values = app->output_values,
			.labels = app->output_labels,
		};
		for (size_t i = 0; i < ARRAY_SIZE(values); ++i) {
			size_t length = strlen(values[i]);

			if (strncmp(cursor, values[i], length) ||
			    (cursor[length] && cursor[length] != ','))
				continue;
			app->output_values[app->output_choice.count] =
				values[i];
			app->output_labels[app->output_choice.count++] =
				labels[i];
			cursor += length;
			if (*cursor == ',')
				++cursor;
		}
		if (*cursor)
			app->output_choice.count = 0;
		app->output = fplinux_settings_choice_find(
			app->output_values, app->output_choice.count,
			app->output_value);
		show_sound(app);
		return;
	} else if (!strcmp(name, "audio_output")) {
		snprintf(app->output_value, sizeof(app->output_value), "%s",
			 value);
		app->output = fplinux_settings_choice_find(
			app->output_values, app->output_choice.count, value);
		show_sound(app);
		return;
	} else if (!strcmp(name, "brightness")) {
		return;
	} else if (!strcmp(name, "screen_off_seconds")) {
		app->screen_off = fplinux_settings_choice_find(
			fplinux_settings_screen_off_values,
			FPLINUX_SETTINGS_SCREEN_OFF_COUNT, value);
	} else if (!strcmp(name, "auto_lock_seconds")) {
		app->auto_lock = fplinux_settings_choice_find(
			fplinux_settings_auto_lock_values,
			FPLINUX_SETTINGS_AUTO_LOCK_COUNT, value);
	} else if (!strcmp(name, "locale")) {
		/* The runtime has taken the new language from this reply. */
		if (strcmp(app->locale, fplinux_ui_locale(app->ui)) ||
		    (app->choice_pending && !app->choice &&
		     !app->input_language))
			rebuild(app);
		return;
	} else if (!strcmp(name, "input_locale")) {
		app->input_unavailable = false;
		show_input(app);
		show_root(app);
		return;
	} else if (!strcmp(name, "time")) {
		/* The clock is set: show it and wait for its next minute. */
		show_clock(app);
		schedule_clock(app);
		return;
	} else if (!strcmp(name, "theme")) {
		/*
		 * The runtime has taken the new theme from this reply; the
		 * screens keep the colours they were made with until rebuilt.
		 * A reply also finishes the waiting selector in the actual theme.
		 */
		if (theme_kind(app) != app->theme ||
		    (app->choice_pending && app->choice == &theme_choice)) {
			rebuild(app);
			return;
		}
	} else {
		return;
	}
	show_display(app);
	show_root(app);
}

/* Known setting names are product labels, never protocol identifiers. */
static const char *setting_title(const char *name)
{
	if (!strcmp(name, "brightness") || !strcmp(name, "brightness_preview"))
		return _("Brightness");
	if (!strcmp(name, "volume") || !strcmp(name, "volume_preview") ||
	    !strcmp(name, "volume_commit"))
		return _("Media volume");
	if (!strncmp(name, "alarm_volume", 12))
		return _("Alarm volume");
	if (!strcmp(name, "audio_output") || !strcmp(name, "audio_outputs"))
		return _("Output");
	if (!strcmp(name, "screen_off_seconds"))
		return _("Screen timeout");
	if (!strcmp(name, "auto_lock_seconds"))
		return _("Auto-lock");
	if (!strcmp(name, "locale"))
		return _("Language");
	if (!strcmp(name, "input_locale"))
		return _("Input language");
	if (!strcmp(name, "theme"))
		return _("Theme");
	if (!strcmp(name, "time"))
		return _("Date and time");
	return NULL;
}

static void notice_closed(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int command_id,
			  void *context)
{
	struct settings_app *app = context;

	(void)ui;
	(void)screen;
	(void)command_id;
	app->notice_open = false;
	app->notice_screen = NULL;
}

static void show_notice(struct settings_app *app)
{
	struct fplinux_ui_screen *alert;
	const char *title = setting_title(app->notice_setting);
	const char *text = _(notice_texts[app->notice]);
	char storage_text[384];
	const struct fplinux_ui_command close = {
		.id = COMMAND_CLOSE,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Close"),
		.icon = fplinux_ui_glyph(app->ui, "back", FPLINUX_UI_GLYPH_ROW),
	};

	if (app->notice == NOTICE_NOT_SAVED) {
		if (app->notice_storage.state ==
		    FPLINUX_STORAGE_PUBLISHED_UNSYNCED)
			text = _(
				"The setting is applied. Saving is not confirmed.");
		snprintf(storage_text, sizeof(storage_text), "%s\n%s", text,
			 fplinux_ui_storage_error(app->notice_storage));
		text = storage_text;
	}
	alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_ERROR,
		title ? title : _(notice_titles[app->notice]), text, 0);
	if (alert) {
		fplinux_ui_screen_add_command(alert, &close);
		fplinux_ui_screen_set_listener(alert, notice_closed, NULL, app);
		app->notice_open = true;
		app->notice_screen = alert;
		fplinux_ui_push(app->ui, alert);
	}
}

static void recover_choice(struct settings_app *app)
{
	if (!app->choice_requery ||
	    fplinux_ui_lifecycle(app->ui) != FPLINUX_UI_FOREGROUND ||
	    !fplinux_ui_is_visible(app->ui))
		return;
	app->choice_requery = false;
	query_setting(app, app->choice ? app->choice->setting : "locale");
}

static void show_error(struct settings_app *app,
		       const struct fplinux_ui_session_message *message)
{
	enum notice notice = NOTICE_FAILED;

	fprintf(stderr, "fplinux-settings: %s=\"%s\" failed: %s\n",
		message->name, message->value, message->reason);
	if (!strcmp(message->name, "volume_preview") ||
	    !strcmp(message->name, "alarm_volume_preview")) {
		if (!message->value[0] || !app->volume_previewing)
			return;
		end_volume_preview(app);
		if (app->volume_form)
			fplinux_ui_form_set_value(
				app->volume_form, app->volume_gauge,
				(int)app->volume[app->volume_class]);
	} else if (!strcmp(message->name, "brightness_preview")) {
		/* One notice for the steps that were on their way. */
		if (!message->value[0] || !app->previewing)
			return;
		/* The gauge returns to the stored level, shown again. */
		end_preview(app);
		if (app->brightness_form)
			fplinux_ui_form_set_value(app->brightness_form,
						  app->gauge_item,
						  (int)app->brightness);
	} else if (!message->value[0]) {
		if (app->choice_pending &&
		    !strcmp(message->reason, "not-foreground"))
			app->choice_requery = true;
		/* A failed query leaves the value unknown. */
		if (!strcmp(message->name, "input_locale")) {
			app->input_unavailable = true;
			show_input(app);
			show_root(app);
		} else if (!strcmp(message->name, "audio_outputs") ||
			   !strcmp(message->name, "audio_output")) {
			app->output_choice.count = 0;
			show_sound(app);
		}
		return;
	}
	if (!strcmp(message->reason, "unavailable"))
		notice = NOTICE_UNAVAILABLE;
	else if (!strcmp(message->reason, "save-failed"))
		notice = NOTICE_NOT_SAVED;
	else if (!strcmp(message->name, "time") &&
		 !strcmp(message->reason, "failed"))
		/* The clock may be set before formatting the reply fails. */
		notice = NOTICE_TIME_FAILED;
	if (!strcmp(message->name, "time") && app->date_form) {
		app->date_show_current = true;
		show_clock(app);
	}
	if (app->notice_open)
		fplinux_ui_pop(app->ui);
	app->notice = notice;
	app->notice_storage = (struct fplinux_storage_result){
		.state = message->storage_state,
		.error = message->storage_error,
	};
	snprintf(app->notice_setting, sizeof(app->notice_setting), "%s",
		 message->name);
	show_notice(app);
	if (!message->applied && message->name[0] &&
	    strcmp(message->name, "time") &&
	    strcmp(message->name, "brightness_preview") &&
	    strcmp(message->name, "brightness") &&
	    strncmp(message->name, "volume", 6) &&
	    strncmp(message->name, "alarm_volume", 12)) {
		if (app->choice_pending) {
			app->choice_requery = true;
			recover_choice(app);
		} else if (fplinux_ui_lifecycle(app->ui) ==
				   FPLINUX_UI_FOREGROUND &&
			   fplinux_ui_is_visible(app->ui)) {
			query_setting(app, message->name);
		}
	}
}

static void handle_event(struct fplinux_ui *ui,
			 const struct fplinux_ui_event *event, void *context)
{
	struct settings_app *app = context;
	const struct fplinux_ui_session_message *message = event->session;

	switch (event->type) {
	case FPLINUX_UI_EVENT_SYSTEM_LEVELS:
		receive_system_levels(app);
		break;
	case FPLINUX_UI_EVENT_SESSION:
		if (message->type == FPLINUX_UI_SESSION_WIFI_STATE ||
		    message->type == FPLINUX_UI_SESSION_WIFI_RESULT) {
			receive_wifi(app, message);
			break;
		}
		if (app->volume_pending &&
		    (!strcmp(message->name,
			     volume_setting_name(app->volume_class, false)) ||
		     !strcmp(message->name,
			     app->volume_class == SOUND_CLASS_MEDIA ?
				     "volume_commit" :
				     "alarm_volume_commit")) &&
		    (message->type == FPLINUX_UI_SESSION_SETTING ||
		     message->type == FPLINUX_UI_SESSION_ERROR)) {
			app->volume_pending = false;
			if (message->applied) {
				app->volume_previewing = false;
				close_screen(app, &app->volume_form);
			} else if (app->volume_form)
				fplinux_ui_screen_set_header_status(
					app->volume_form, NULL, NULL);
		}
		if (message->type == FPLINUX_UI_SESSION_ERROR &&
		    message->value[0])
			app->root_requested = false;
		/* Only one time mutation is outstanding; its reply reports current time. */
		if (app->date_pending && !strcmp(message->name, "time") &&
		    (message->type == FPLINUX_UI_SESSION_SETTING ||
		     message->type == FPLINUX_UI_SESSION_ERROR)) {
			app->date_pending = false;
			date_commands(app);
			if (message->type == FPLINUX_UI_SESSION_SETTING &&
			    app->date_form &&
			    app->date_revision == app->sent_date_revision) {
				app->date_edited = false;
				app->time_edited = false;
				if (!fplinux_ui_form_is_editing(
					    app->date_form) &&
				    fplinux_ui_current(ui) == app->date_form)
					close_screen(app, &app->date_form);
			}
			show_clock(app);
			schedule_clock(app);
		}
		if (message->applied && strcmp(message->name, "time") &&
		    strcmp(message->name, "brightness_preview") &&
		    strcmp(message->name, "volume_preview") &&
		    strcmp(message->name, "alarm_volume_preview"))
			app->unsaved = message->storage_error != 0;
		if (message->type == FPLINUX_UI_SESSION_SETTING)
			apply_setting(app, message->name, message->value);
		else if (message->type == FPLINUX_UI_SESSION_ERROR) {
			if (message->applied)
				apply_setting(app, message->name,
					      message->value);
			show_error(app, message);
		}
		if (app->root_requested &&
		    message->type == FPLINUX_UI_SESSION_SETTING &&
		    !app->date_pending && !app->choice_pending &&
		    !app->volume_pending)
			open_root(app);
		break;
	case FPLINUX_UI_EVENT_ACTIVATION:
		if (event->activation == FPLINUX_UI_ACTIVATION_OPEN)
			open_root(app);
		break;
	case FPLINUX_UI_EVENT_LIFECYCLE:
		if (fplinux_ui_lifecycle(ui) == FPLINUX_UI_BACKGROUND) {
			fplinux_settings_wifi_background(app->wifi);
			/* The manager restores the level when the task is parked. */
			app->previewing = false;
			app->volume_previewing = false;
			if (app->volume_form)
				fplinux_ui_form_set_value(
					app->volume_form, app->volume_gauge,
					(int)app->volume[app->volume_class]);
			if (app->brightness_form)
				fplinux_ui_form_set_value(app->brightness_form,
							  app->gauge_item,
							  (int)app->brightness);
		} else {
			recover_choice(app);
			/* The system bus may have started or restarted since. */
			if (app->bluetooth)
				fplinux_bluez_adapter_connect(app->bluetooth);
		}
		break;
	case FPLINUX_UI_EVENT_VISIBILITY:
		/* Hidden: no clock updates. Shown: catch up at once. */
		if (event->visible) {
			show_clock(app);
			recover_choice(app);
		}
		schedule_clock(app);
		break;
	case FPLINUX_UI_EVENT_CLOSE:
		/*
		 * A previewed level that was never saved is not kept; the
		 * manager also restores it if this message is lost.
		 */
		end_preview(app);
		end_volume_preview(app);
		break;
	case FPLINUX_UI_EVENT_KEY:
		/* The screens take every key. */
		break;
	default:
		/* Settings uses the integrated application surface. */
		break;
	}
	if (app->bluetooth_view)
		fplinux_settings_bluetooth_event(app->bluetooth_view, event);
}

static void storage_result(struct fplinux_ui *ui,
			   const struct fplinux_volume_result *result,
			   void *context)
{
	struct settings_app *app = context;

	(void)ui;
	if (app->bluetooth_view)
		fplinux_settings_bluetooth_storage_result(app->bluetooth_view,
							  result);
}

static void close_bluetooth(struct settings_app *app)
{
	struct fplinux_settings_bluetooth *view = app->bluetooth_view;

	app->bluetooth_view = NULL;
	if (view)
		fplinux_settings_bluetooth_free(view);
	fplinux_bluez_adapter_close(app->bluetooth);
	app->bluetooth = NULL;
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-settings",
		.description = "Adjust phone settings",
	};
	struct settings_app app = {
		.screen_off = FPLINUX_SETTINGS_SCREEN_OFF_COUNT,
		.auto_lock = FPLINUX_SETTINGS_AUTO_LOCK_COUNT,
	};
	struct fplinux_ui_screen *root;
	int result;

	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	app.ui = fplinux_ui_open(APP_ID, handle_event, &app);
	if (!app.ui) {
		perror("fplinux-settings: open UI");
		return 1;
	}
	/* The theme came with CONFIG, as the language did. */
	app.theme = theme_kind(&app);
	snprintf(app.locale, sizeof(app.locale), "%s",
		 fplinux_ui_locale(app.ui));
	fplinux_ui_set_close_handler(app.ui, prepare_close, &app);
	fplinux_ui_set_storage_handler(app.ui, storage_result, &app);
	app.clock_timer = lv_timer_create(clock_tick, 60000, &app);
	lv_timer_pause(app.clock_timer);
	app.bluetooth = fplinux_bluez_adapter_open(
		&(const struct fplinux_bluez_adapter_listener){
			.changed = bluetooth_changed,
			.finished = bluetooth_finished,
			.context = &app,
		});
	if (app.bluetooth) {
		if (!fplinux_ui_watch_fd(
			    app.ui, fplinux_bluez_adapter_fd(app.bluetooth),
			    POLLIN, bluetooth_ready, &app)) {
			perror("fplinux-settings: watch Bluetooth");
			lv_timer_delete(app.clock_timer);
			close_bluetooth(&app);
			fplinux_ui_close(app.ui);
			return 1;
		}
		fplinux_bluez_adapter_connect(app.bluetooth);
	}
	root = create_root(&app);
	if (!root) {
		lv_timer_delete(app.clock_timer);
		close_bluetooth(&app);
		fplinux_ui_close(app.ui);
		return 1;
	}
	fplinux_ui_push(app.ui, root);
	app.root = root;
	show_root(&app);
	/* Levels arrive in SYSTEM_LEVELS; CONFIG supplied language and theme. */
	query_setting(&app, "screen_off_seconds");
	query_setting(&app, "auto_lock_seconds");
	query_setting(&app, "input_locale");
	result = fplinux_ui_run(app.ui);
	lv_timer_delete(app.clock_timer);
	close_bluetooth(&app);
	fplinux_settings_wifi_free(app.wifi);
	fplinux_ui_close(app.ui);
	return result < 0;
}
