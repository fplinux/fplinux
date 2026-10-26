/* SPDX-License-Identifier: 0BSD */
/*
 * Language and data: the system language, translations, dates and numbers
 * in its formats, preferences and the application's state directory.
 */
/* fsync(), rename() and the other POSIX.1-2008 file functions. */
#define _POSIX_C_SOURCE 200809L
#include "apidemo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEXT_BYTES 512
/* 30 August 2026, 14:05 in the local time zone, for the fixed examples. */
#define FIXED_YEAR 2026
#define FIXED_MONTH 8
#define FIXED_DAY 30
#define FIXED_HOUR 14
#define FIXED_MINUTE 5

/* Preference keys of this category. */
#define PREF_NAME "locale.name"
#define PREF_COUNT "locale.count"
#define PREF_NAME_CHARS 24

/*
 * musl has no pgettext(). One English text with two translations is looked
 * up as "context\004text", which a msgctxt entry of the catalogue provides;
 * without a translation the text after \004 is shown.
 */
static const char *context_gettext(const char *key)
{
	const char *text = gettext(key);

	return text == key ? strchr(key, '\004') + 1 : text;
}

#define C_(context, text) context_gettext(context "\004" text)

static struct fplinux_ui_screen *new_form(struct apidemo *app,
					  const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form)
		apidemo_error(app, "fplinux_ui_form_new");
	return form;
}

/* The system language */

static void open_language(struct apidemo *app,
			  const struct apidemo_entry *entry)
{
	static const int counts[] = { 1, 2, 5, 21 };
	struct fplinux_ui_screen *form = new_form(app, entry);
	const char *lang = getenv("LANG");
	char text[TEXT_BYTES];
	size_t used = 0;

	if (!form)
		return;
	/* A code such as "en" or "ru": any installed language may come. */
	fplinux_ui_form_append_string(form, "fplinux_ui_locale()",
				      fplinux_ui_locale(app->ui));
	fplinux_ui_form_append_string(form, "LANG", lang ? lang : "");
	fplinux_ui_form_append_string(
		form, "gettext()",
		_("This text comes from the application's catalogue."));
	/* The catalogue's Plural-Forms chooses the form for each count. */
	text[0] = '\0';
	for (size_t i = 0; i < APIDEMO_COUNT(counts) && used < sizeof(text);
	     i++) {
		int written = snprintf(text + used, sizeof(text) - used, "%s",
				       used ? ", " : "");

		if (written < 0)
			break;
		used += (size_t)written;
		if (used >= sizeof(text))
			break;
		written = snprintf(text + used, sizeof(text) - used,
				   ngettext("%d file", "%d files", counts[i]),
				   counts[i]);
		if (written < 0)
			break;
		used += (size_t)written;
	}
	fplinux_ui_form_append_string(form, "ngettext()", text);
	/* TRANSLATORS: two meanings of one English word, kept apart by
	 * their msgctxt: a soft-key word and the weather. */
	snprintf(text, sizeof(text), "%s / %s", C_("soft key", "Clear"),
		 C_("weather", "Clear"));
	fplinux_ui_form_append_string(form, "msgctxt", text);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, apidemo_back_listener, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* Dates */

/* A fixed local moment, so that the formats can be compared. */
static time_t fixed_moment(void)
{
	struct tm moment = {
		.tm_year = FIXED_YEAR - 1900,
		.tm_mon = FIXED_MONTH - 1,
		.tm_mday = FIXED_DAY,
		.tm_hour = FIXED_HOUR,
		.tm_min = FIXED_MINUTE,
		.tm_isdst = -1,
	};

	return mktime(&moment);
}

/* Formats when in style, or names the error. */
static void format_date(struct apidemo *app, time_t when,
			enum fplinux_ui_date_style style, char *out,
			size_t size)
{
	if (!fplinux_ui_format_date(app->ui, when, style, out, size))
		snprintf(out, size, "false, %s", apidemo_errno_name(errno));
}

static struct {
	struct fplinux_ui_screen *form;
	size_t now[3];
} dates;

static const enum fplinux_ui_date_style date_styles[] = {
	FPLINUX_UI_DATE_LONG,
	FPLINUX_UI_DATE_SHORT,
	FPLINUX_UI_DATE_CLOCK,
};
static const char *const date_style_names[] = {
	"FPLINUX_UI_DATE_LONG",
	"FPLINUX_UI_DATE_SHORT",
	"FPLINUX_UI_DATE_CLOCK",
};

static void show_now(struct apidemo *app)
{
	char text[TEXT_BYTES];
	time_t now = time(NULL);

	for (size_t i = 0; i < APIDEMO_COUNT(date_styles); i++) {
		format_date(app, now, date_styles[i], text, sizeof(text));
		fplinux_ui_form_set_string(dates.form, dates.now[i], text);
	}
}

enum { COMMAND_NOW = 1 };

static void dates_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	(void)form;
	if (command_id == COMMAND_NOW) {
		show_now(context);
	} else if (command_id == APIDEMO_COMMAND_BACK) {
		dates.form = NULL;
		fplinux_ui_go_back(ui);
	}
}

static void open_dates(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command now_command = {
		.id = COMMAND_NOW,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Update"),
		.priority = 0,
		.icon = apidemo_glyph(app, "refresh"),
	};
	struct fplinux_ui_screen *form = new_form(app, entry);
	time_t fixed = fixed_moment();
	char text[TEXT_BYTES];

	if (!form)
		return;
	dates.form = form;
	fplinux_ui_form_append_string(form, NULL,
				      _("The time now, in the system "
					"language:"));
	for (size_t i = 0; i < APIDEMO_COUNT(date_styles); i++)
		dates.now[i] = fplinux_ui_form_append_string(
			form, date_style_names[i], NULL);
	show_now(app);
	fplinux_ui_form_append_string(form, NULL, _("30 August 2026, 14:05:"));
	for (size_t i = 0; i < APIDEMO_COUNT(date_styles); i++) {
		format_date(app, fixed, date_styles[i], text, sizeof(text));
		fplinux_ui_form_append_string(form, date_style_names[i], text);
	}
	fplinux_ui_screen_add_command(form, &now_command);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, dates_command, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* Numbers */

static void add_number(struct apidemo *app, struct fplinux_ui_screen *form,
		       double value, int decimals, size_t size)
{
	char label[TEXT_BYTES];
	char text[TEXT_BYTES];

	snprintf(label, sizeof(label), "%g, %d", value, decimals);
	if (!fplinux_ui_format_number(app->ui, value, decimals, text,
				      size < sizeof(text) ? size :
							    sizeof(text)))
		snprintf(text, sizeof(text), "false, %s",
			 apidemo_errno_name(errno));
	fplinux_ui_form_append_string(form, label, text);
}

static void open_numbers(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form = new_form(app, entry);
	char separator[2] = { 0 };

	if (!form)
		return;
	/* '.' or ',': printf() always writes '.', in every language. */
	separator[0] = fplinux_ui_decimal_separator(app->ui);
	fplinux_ui_form_append_string(form, "fplinux_ui_decimal_separator()",
				      separator);
	fplinux_ui_form_append_string(form, NULL,
				      _("fplinux_ui_format_number(value, "
					"decimals), without digit grouping:"));
	add_number(app, form, 1234.5, 0, TEXT_BYTES);
	add_number(app, form, 1234.5, 2, TEXT_BYTES);
	add_number(app, form, -0.25, 2, TEXT_BYTES);
	add_number(app, form, 3.14159265358979, 9, TEXT_BYTES);
	/* Errors: ten decimals, and a buffer of 4 bytes. */
	add_number(app, form, 1.5, 10, TEXT_BYTES);
	add_number(app, form, 1234.5, 2, 4);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, apidemo_back_listener, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* Preferences */

enum {
	COMMAND_SAVE = 1,
	COMMAND_REMOVE,
};

static struct {
	struct fplinux_ui_screen *form;
	size_t name;
	size_t count;
	size_t stored;
	size_t memory;
	size_t result;
	char draft_name[4 * PREF_NAME_CHARS + 1];
	int draft_count;
	bool has_draft;
} prefs_demo;

static struct fplinux_storage_result last_save = {
	.state = FPLINUX_STORAGE_DURABLE,
};
static bool save_attempted;

/* The active handle and a new file reader establish different facts. */
static void show_stored(struct apidemo *app)
{
	struct fplinux_ui_prefs *file;
	char text[TEXT_BYTES];

	if (!prefs_demo.form)
		return;
	snprintf(text, sizeof(text), "%s = \"%s\"\n%s = %lld", PREF_NAME,
		 fplinux_ui_prefs_get(app->prefs, PREF_NAME, ""), PREF_COUNT,
		 fplinux_ui_prefs_get_int(app->prefs, PREF_COUNT, 0));
	fplinux_ui_form_set_string(prefs_demo.form, prefs_demo.memory, text);
	file = fplinux_ui_prefs_open(APIDEMO_APP_ID);
	if (file) {
		snprintf(text, sizeof(text), "%s = \"%s\"\n%s = %lld",
			 PREF_NAME, fplinux_ui_prefs_get(file, PREF_NAME, ""),
			 PREF_COUNT,
			 fplinux_ui_prefs_get_int(file, PREF_COUNT, 0));
	} else {
		snprintf(text, sizeof(text), "NULL, %s",
			 apidemo_errno_name(errno));
	}
	fplinux_ui_prefs_close(file);
	fplinux_ui_form_set_string(prefs_demo.form, prefs_demo.stored, text);
	if (!save_attempted)
		snprintf(text, sizeof(text), "%s", _("No save attempted"));
	else if (last_save.state == FPLINUX_STORAGE_DURABLE)
		snprintf(text, sizeof(text), "FPLINUX_STORAGE_DURABLE");
	else
		snprintf(text, sizeof(text), "%s, %s\n%s",
			 last_save.state == FPLINUX_STORAGE_UNPUBLISHED ?
				 "FPLINUX_STORAGE_UNPUBLISHED" :
				 "FPLINUX_STORAGE_PUBLISHED_UNSYNCED",
			 apidemo_errno_name(last_save.error),
			 fplinux_ui_storage_error(last_save));
	fplinux_ui_form_set_string(prefs_demo.form, prefs_demo.result, text);
}

static void record_save_result(struct apidemo *app,
			       struct fplinux_storage_result result,
			       bool interactive)
{
	if (result.state != FPLINUX_STORAGE_DURABLE &&
	    (interactive || last_save.state != result.state ||
	     last_save.error != result.error)) {
		errno = result.error;
		if (interactive) {
			struct fplinux_ui_screen *alert =
				apidemo_error(app, "fplinux_ui_prefs_save");

			apidemo_protect_error(app, alert);
		} else
			perror("fplinux-example-apidemo: fplinux_ui_prefs_save");
	} else if (result.state == FPLINUX_STORAGE_DURABLE && last_save.error) {
		fputs("fplinux-example-apidemo: preferences saved\n", stderr);
	}
	last_save = result;
	save_attempted = true;
	show_stored(app);
}

struct fplinux_storage_result apidemo_save_prefs(struct apidemo *app,
						 bool interactive)
{
	struct fplinux_storage_result result =
		fplinux_ui_prefs_save(app->prefs);

	record_save_result(app, result, interactive);
	return result;
}

/*
 * The values stay in memory until fplinux_ui_prefs_save(), which replaces
 * the file atomically; the application also saves when it is hidden and on
 * CLOSE (fplinux-example-apidemo.c).
 */
static void save_prefs(struct apidemo *app)
{
	const char *name =
		fplinux_ui_form_get_text(prefs_demo.form, prefs_demo.name);
	int count =
		fplinux_ui_form_get_value(prefs_demo.form, prefs_demo.count);
	if (!fplinux_ui_prefs_set(app->prefs, PREF_NAME, name ? name : "") ||
	    !fplinux_ui_prefs_set_int(app->prefs, PREF_COUNT, count)) {
		struct fplinux_storage_result result = {
			.state = FPLINUX_STORAGE_UNPUBLISHED,
			.error = errno,
		};

		record_save_result(app, result, true);
		return;
	}
	apidemo_save_prefs(app, true);
}

static void remove_prefs(struct apidemo *app)
{
	fplinux_ui_prefs_remove(app->prefs, PREF_NAME);
	fplinux_ui_prefs_remove(app->prefs, PREF_COUNT);
	apidemo_save_prefs(app, true);
}

static void prefs_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	struct apidemo *app = context;

	(void)form;
	switch (command_id) {
	case COMMAND_SAVE:
		save_prefs(app);
		break;
	case COMMAND_REMOVE:
		remove_prefs(app);
		break;
	case APIDEMO_COMMAND_BACK:
		prefs_demo.form = NULL;
		prefs_demo.has_draft = false;
		fplinux_ui_go_back(ui);
		break;
	default:
		break;
	}
}

static void open_prefs(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command commands[] = {
		{ .id = COMMAND_SAVE,
		  .type = FPLINUX_UI_COMMAND_OK,
		  .label = _("Save"),
		  .priority = 0,
		  .icon = apidemo_glyph(app, "save") },
		{ .id = COMMAND_REMOVE,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = _("Remove"),
		  .priority = 1,
		  .icon = apidemo_glyph(app, "delete") },
	};
	struct fplinux_ui_screen *form;
	const char *name;
	int count;

	if (!app->prefs) {
		apidemo_show(app, _(entry->title),
			     _("The preferences could not be read at start."));
		return;
	}
	form = new_form(app, entry);
	if (!form)
		return;
	prefs_demo.form = form;
	name = prefs_demo.has_draft ?
		       prefs_demo.draft_name :
		       fplinux_ui_prefs_get(app->prefs, PREF_NAME, "");
	count = prefs_demo.has_draft ?
			prefs_demo.draft_count :
			(int)fplinux_ui_prefs_get_int(app->prefs, PREF_COUNT,
						      0);
	prefs_demo.name = fplinux_ui_form_append_text_field(
		form, _("Name"), name, PREF_NAME_CHARS, FPLINUX_UI_TEXT_ANY);
	prefs_demo.count = fplinux_ui_form_append_gauge(form, _("Count"), 0, 20,
							count, true);
	prefs_demo.stored =
		fplinux_ui_form_append_string(form, _("Stored"), NULL);
	prefs_demo.memory =
		fplinux_ui_form_append_string(form, _("In memory"), NULL);
	prefs_demo.result =
		fplinux_ui_form_append_string(form, _("Save result"), NULL);
	show_stored(app);
	for (size_t i = 0; i < APIDEMO_COUNT(commands); i++)
		fplinux_ui_screen_add_command(form, &commands[i]);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, prefs_command, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* The state directory */

enum { COMMAND_WRITE = 1 };

static struct {
	struct fplinux_ui_screen *form;
	size_t result;
} state_demo;

/*
 * Replaces a file atomically: write and sync a
 * temporary file, rename it over the old one, then sync the directory.
 * If directory sync fails, the file is replaced but durability is unconfirmed.
 */
static struct fplinux_storage_result write_note(const char *directory,
						char *path, size_t size)
{
	char temporary[TEXT_BYTES];
	const char *text = "API Demos\n";
	size_t length = strlen(text);
	size_t offset = 0;
	struct fplinux_storage_result result = {
		.state = FPLINUX_STORAGE_UNPUBLISHED,
	};
	int fd;

	if (snprintf(path, size, "%s/note.txt", directory) >= (int)size ||
	    snprintf(temporary, sizeof(temporary), "%s/.note.txt.new",
		     directory) >= (int)sizeof(temporary)) {
		result.error = ENAMETOOLONG;
		return result;
	}
	fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		result.error = errno;
		return result;
	}
	while (offset < length) {
		ssize_t written = write(fd, text + offset, length - offset);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			result.error = written < 0 ? errno : EIO;
			break;
		}
		offset += (size_t)written;
	}
	if (!result.error && fsync(fd) < 0)
		result.error = errno;
	if (close(fd) < 0 && !result.error)
		result.error = errno;
	if (!result.error && rename(temporary, path) < 0)
		result.error = errno;
	if (result.error) {
		unlink(temporary);
		return result;
	}
	result.state = FPLINUX_STORAGE_PUBLISHED_UNSYNCED;
	fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		result.error = errno;
		return result;
	}
	if (fsync(fd) < 0)
		result.error = errno;
	if (close(fd) < 0 && !result.error)
		result.error = errno;
	if (!result.error)
		result.state = FPLINUX_STORAGE_DURABLE;
	return result;
}

static void state_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	char directory[TEXT_BYTES];
	char path[TEXT_BYTES];
	char text[TEXT_BYTES];
	struct fplinux_storage_result result = {
		.state = FPLINUX_STORAGE_UNPUBLISHED,
	};

	(void)form;
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK) {
		state_demo.form = NULL;
		fplinux_ui_go_back(ui);
		return;
	}
	if (command_id != COMMAND_WRITE)
		return;
	if (!fplinux_ui_state_dir(APIDEMO_APP_ID, directory, sizeof(directory)))
		result.error = errno;
	else
		result = write_note(directory, path, sizeof(path));
	if (result.state == FPLINUX_STORAGE_DURABLE)
		snprintf(text, sizeof(text), _("Written: %s"), path);
	else
		snprintf(text, sizeof(text), "%s, %s\n%s",
			 result.state == FPLINUX_STORAGE_UNPUBLISHED ?
				 "FPLINUX_STORAGE_UNPUBLISHED" :
				 "FPLINUX_STORAGE_PUBLISHED_UNSYNCED",
			 apidemo_errno_name(result.error),
			 fplinux_ui_storage_error(result));
	fplinux_ui_form_set_string(state_demo.form, state_demo.result, text);
}

/* The directory and the errors of fplinux_ui_state_dir(). */
static void add_state_dir(struct fplinux_ui_screen *form, const char *label,
			  const char *app_id, size_t capacity)
{
	char path[TEXT_BYTES];

	if (capacity > sizeof(path))
		capacity = sizeof(path);
	if (!fplinux_ui_state_dir(app_id, path, capacity))
		snprintf(path, sizeof(path), "false, %s",
			 apidemo_errno_name(errno));
	fplinux_ui_form_append_string(form, label, path);
}

static void open_state(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command write_command = {
		.id = COMMAND_WRITE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Write"),
		.priority = 0,
		.icon = apidemo_glyph(app, "save"),
	};
	struct fplinux_ui_screen *form = new_form(app, entry);

	if (!form)
		return;
	state_demo.form = form;
	add_state_dir(form, APIDEMO_APP_ID, APIDEMO_APP_ID, TEXT_BYTES);
	add_state_dir(form, _("Empty ID"), "", TEXT_BYTES);
	add_state_dir(form, _("ID with /"), "org/example", TEXT_BYTES);
	add_state_dir(form, _("Into 8 bytes"), APIDEMO_APP_ID, 8);
	state_demo.result = fplinux_ui_form_append_string(
		form, _("note.txt"),
		_("OK writes it there, replacing it atomically."));
	fplinux_ui_screen_add_command(form, &write_command);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, state_command, NULL, app);
	fplinux_ui_push(app->ui, form);
}

static void locale_cleanup(struct apidemo *app)
{
	(void)app;
	dates.form = NULL;
	if (prefs_demo.form) {
		const char *name = fplinux_ui_form_get_text(prefs_demo.form,
							    prefs_demo.name);

		/* OPEN keeps edits in memory without applying or saving them. */
		snprintf(prefs_demo.draft_name, sizeof(prefs_demo.draft_name),
			 "%s", name ? name : "");
		prefs_demo.draft_count = fplinux_ui_form_get_value(
			prefs_demo.form, prefs_demo.count);
		prefs_demo.has_draft = true;
	}
	prefs_demo.form = NULL;
	state_demo.form = NULL;
}

static const struct apidemo_entry entries[] = {
	{ N_("System language"),
	  "fplinux_ui_locale · gettext · ngettext · msgctxt", open_language, 0,
	  N_("Use translated text and plurals") },
	{ N_("Dates"),
	  "fplinux_ui_format_date · FPLINUX_UI_DATE_LONG · "
	  "FPLINUX_UI_DATE_SHORT · FPLINUX_UI_DATE_CLOCK",
	  open_dates, 0, N_("Format fixed and current dates") },
	{ N_("Numbers"),
	  "fplinux_ui_format_number · fplinux_ui_decimal_separator",
	  open_numbers, 0, N_("Display numbers in the system language") },
	{ N_("Preferences"),
	  "fplinux_ui_prefs_open · fplinux_ui_prefs_get · fplinux_ui_prefs_set · "
	  "fplinux_ui_prefs_save · fplinux_ui_prefs_close",
	  open_prefs, 0, N_("Compare memory, the file and confirmed saving") },
	{ N_("State directory"), "fplinux_ui_state_dir · fsync · rename",
	  open_state, 0, N_("Write a private state file atomically") },
};

const struct apidemo_category apidemo_locale = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.cleanup = locale_cleanup,
};
