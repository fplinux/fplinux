/* SPDX-License-Identifier: 0BSD */
/*
 * Forms: the Form screen of fplinux-ui-screen.h with every item type.
 *
 * Each demo is one open function that builds a form, adds its commands and
 * listeners and pushes it, followed by those listeners. One demo runs at a
 * time, so the item indices a demo needs are kept in static variables next
 * to it. Most forms begin with a string item that shows what the change
 * listener reported last: the user's changes are reported, the
 * application's own changes through the fplinux_ui_form_set_*() functions
 * are not.
 */
#include "apidemo.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Command ids of the demo screens; each screen uses a few of them. */
enum {
	COMMAND_CHANGE = 1,
	COMMAND_REMOVE_TEXT,
	COMMAND_SHOW,
	COMMAND_CALL,
	COMMAND_LOCK,
	COMMAND_MASK,
	COMMAND_ANY,
	COMMAND_NUMERIC,
	COMMAND_DECIMAL,
	COMMAND_PHONE,
	COMMAND_CHOOSE_ALL,
	COMMAND_RESET,
	COMMAND_START,
	COMMAND_NOW,
	COMMAND_MOVE_UP,
	COMMAND_MOVE_DOWN,
	COMMAND_HIDE,
	COMMAND_DELETE,
	COMMAND_INSERT,
	COMMAND_SHOW_HIDDEN,
	COMMAND_FOCUS_LAST,
	COMMAND_LIST_ITEMS,
	COMMAND_DELETE_ALL,
	COMMAND_SEND,
	COMMAND_TODAY,
	COMMAND_MUTE,
	COMMAND_PLAY,
	COMMAND_CALLS,
	COMMAND_OPEN,
	COMMAND_COPY,
	COMMAND_VIEW,
	COMMAND_SHOW_FOCUS,
	COMMAND_CHANGE_BUTTON,
	COMMAND_ARM,
};

/* Adds a command; the runtime copies the label and decodes the icon. */
static void add_command(struct fplinux_ui_screen *screen, int id,
			enum fplinux_ui_command_type type, const char *label,
			int priority, const void *icon)
{
	const struct fplinux_ui_command command = {
		.id = id,
		.type = type,
		.label = label,
		.priority = priority,
		.icon = icon,
	};

	fplinux_ui_screen_add_command(screen, &command);
}

/* Appends formatted text to a result text, as far as it fits. */
static void append_text(char *text, size_t size, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

static void append_text(char *text, size_t size, const char *format, ...)
{
	size_t used = strlen(text);
	va_list arguments;

	if (used + 1 >= size)
		return;
	va_start(arguments, format);
	vsnprintf(text + used, size - used, format, arguments);
	va_end(arguments);
}

/* The result of a bool call for the result texts: true, or false and errno. */
static void describe_result(char *text, size_t size, bool done, int error)
{
	if (done)
		snprintf(text, size, "true");
	else
		snprintf(text, size, "false, %s", apidemo_errno_name(error));
}

/*
 * The change listener's readout: a string item at the top of the form,
 * which fplinux_ui_form_set_string() rewrites.
 */
static size_t append_status(struct fplinux_ui_screen *form)
{
	return fplinux_ui_form_append_string(form, _("Change listener"),
					     _("Nothing changed yet."));
}

/*
 * A longer text makes the status taller; the form keeps the focused item
 * below it in view.
 */
static void show_status(struct fplinux_ui_screen *form, size_t status,
			const char *format, ...)
	__attribute__((format(printf, 3, 4)));

static void show_status(struct fplinux_ui_screen *form, size_t status,
			const char *format, ...)
{
	char text[160];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	fplinux_ui_form_set_string(form, status, text);
}

/* String items */

static size_t strings_changed_item;
static int strings_changes;

static void strings_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, int command_id,
			    void *context)
{
	char text[64];

	(void)context;
	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_CHANGE:
		++strings_changes;
		snprintf(text, sizeof(text),
			 ngettext("Changed %d time.", "Changed %d times.",
				  (unsigned long)strings_changes),
			 strings_changes);
		fplinux_ui_form_set_string(form, strings_changed_item, text);
		break;
	case COMMAND_REMOVE_TEXT:
		/* NULL or "" leaves the label alone. */
		fplinux_ui_form_set_string(form, strings_changed_item, NULL);
		break;
	default:
		break;
	}
}

static void open_strings(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	fplinux_ui_form_append_string(
		form, _("Label and text"),
		_("A string item shows a label above its text."));
	fplinux_ui_form_append_string(form, _("A label alone"), NULL);
	fplinux_ui_form_append_string(form, NULL,
				      _("A text alone, without a label."));
	/* Neither: an empty item, which only adds the gap between items. */
	fplinux_ui_form_append_string(form, NULL, NULL);
	fplinux_ui_form_append_string(
		form, _("Long text"),
		_("A string item wraps its text onto as many lines as it "
		  "needs. When the form is taller than the screen, Up and "
		  "Down scroll it by a line, since no item here takes the "
		  "focus. Nothing is cut and nothing moves."));
	strings_changes = 0;
	strings_changed_item = fplinux_ui_form_append_string(
		form, _("Set by the application"), _("Not changed yet."));
	/* No item takes the focus, so OK runs the first OK command. */
	add_command(form, COMMAND_CHANGE, FPLINUX_UI_COMMAND_OK, _("Change"), 0,
		    apidemo_glyph(app, "edit"));
	add_command(form, COMMAND_REMOVE_TEXT, FPLINUX_UI_COMMAND_SCREEN,
		    _("Remove text"), 1, apidemo_glyph(app, "backspace"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, strings_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Text fields and passwords */

struct field {
	const char *label;
	/* The text at the start, or NULL. */
	const char *text;
	/* At most this many characters, with a counter; 0 for no limit. */
	size_t max_chars;
	enum fplinux_ui_text_constraint constraint;
};

static const struct field fields[] = {
	{ N_("Any text"), NULL, 24, FPLINUX_UI_TEXT_ANY },
	{ N_("Integer"), NULL, 6, FPLINUX_UI_TEXT_NUMERIC },
	/* A DECIMAL field shows the language's separator: 9,99 in Russian. */
	{ N_("Decimal number"), "9.99", 8, FPLINUX_UI_TEXT_DECIMAL },
	{ N_("Phone number"), NULL, 20, FPLINUX_UI_TEXT_PHONE },
	{ N_("Any text, no limit"), NULL, 0, FPLINUX_UI_TEXT_ANY },
	{ N_("Password"), NULL, 16,
	  FPLINUX_UI_TEXT_ANY | FPLINUX_UI_TEXT_PASSWORD },
	{ N_("PIN"), NULL, 4,
	  FPLINUX_UI_TEXT_NUMERIC | FPLINUX_UI_TEXT_PASSWORD },
	{ N_("Read-only"), N_("This value cannot be edited."), 0,
	  FPLINUX_UI_TEXT_ANY | FPLINUX_UI_TEXT_UNEDITABLE },
};

static size_t field_items[APIDEMO_COUNT(fields)];
static size_t fields_status;

/* A text field reports every edit; a password returns its real text. */
static void fields_changed(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *form, size_t index,
			   void *context)
{
	const char *text = fplinux_ui_form_get_text(form, index);

	(void)ui;
	(void)context;
	show_status(form, fields_status, _("Item %zu: “%s”"), index,
		    text ? text : "");
}

/* A DECIMAL field returns its number with '.', whatever it shows. */
static void show_fields(struct apidemo *app, struct fplinux_ui_screen *form)
{
	char text[512] = "";

	for (size_t i = 0; i < APIDEMO_COUNT(fields); ++i) {
		const char *value =
			fplinux_ui_form_get_text(form, field_items[i]);

		append_text(text, sizeof(text), _("%s: “%s”"),
			    _(fields[i].label), value ? value : "");
		append_text(text, sizeof(text), "%s",
			    i + 1 < APIDEMO_COUNT(fields) ? "\n" : "");
	}
	apidemo_show(app, _("Values"), text);
}

static void fields_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *form, int command_id,
			   void *context)
{
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_SHOW)
		show_fields(context, form);
}

static void open_fields(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	fields_status = append_status(form);
	for (size_t i = 0; i < APIDEMO_COUNT(fields); ++i)
		field_items[i] = fplinux_ui_form_append_text_field(
			form, _(fields[i].label),
			fields[i].text ? _(fields[i].text) : NULL,
			fields[i].max_chars, fields[i].constraint);
	/* A text field has no OK action of its own: OK runs Show. */
	add_command(form, COMMAND_SHOW, FPLINUX_UI_COMMAND_OK, _("Show"), 0,
		    apidemo_glyph(app, "view"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, fields_command, fields_changed,
				       app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Changing the type */

static size_t constraint_status;
static size_t constraint_field;
/* The field's type and modifiers, which the commands change. */
static enum fplinux_ui_text_constraint constraint_type;
static bool constraint_locked;
static bool constraint_masked;

/* The constraint as C code, for the result text. */
static void constraint_name(enum fplinux_ui_text_constraint constraint,
			    char *text, size_t size)
{
	static const char *const types[] = { "ANY", "NUMERIC", "DECIMAL",
					     "PHONE" };
	unsigned int type = (unsigned int)constraint &
			    ~(unsigned int)(FPLINUX_UI_TEXT_PASSWORD |
					    FPLINUX_UI_TEXT_UNEDITABLE);

	snprintf(text, size, "%s%s%s",
		 type < APIDEMO_COUNT(types) ? types[type] : "?",
		 constraint & FPLINUX_UI_TEXT_PASSWORD ? " | PASSWORD" : "",
		 constraint & FPLINUX_UI_TEXT_UNEDITABLE ? " | UNEDITABLE" :
							   "");
}

/* The labels of the two toggles follow the field's state. */
static void show_toggles(struct apidemo *app, struct fplinux_ui_screen *form)
{
	/* Adding a command id again replaces the command. */
	add_command(form, COMMAND_LOCK, FPLINUX_UI_COMMAND_SCREEN,
		    constraint_locked ? _("Make editable") :
					_("Make read-only"),
		    1, apidemo_glyph(app, constraint_locked ? "edit" : "lock"));
	add_command(form, COMMAND_MASK, FPLINUX_UI_COMMAND_SCREEN,
		    constraint_masked ? _("Show the text") : _("Hide the text"),
		    2, apidemo_glyph(app, constraint_masked ? "view" : "hide"));
}

/*
 * Applies a type with the current modifiers. The call fails with EINVAL
 * when the text does not suit the type, such as letters for NUMERIC, and
 * the field keeps the type it had.
 */
static void apply_constraint(struct apidemo *app,
			     struct fplinux_ui_screen *form,
			     enum fplinux_ui_text_constraint type, bool locked,
			     bool masked)
{
	enum fplinux_ui_text_constraint constraint = type;
	char name[64];
	char result[32];
	bool done;

	if (locked)
		constraint |= FPLINUX_UI_TEXT_UNEDITABLE;
	if (masked)
		constraint |= FPLINUX_UI_TEXT_PASSWORD;
	errno = 0;
	done = fplinux_ui_form_set_constraint(form, constraint_field,
					      constraint);
	describe_result(result, sizeof(result), done, errno);
	constraint_name(constraint, name, sizeof(name));
	show_status(form, constraint_status,
		    "fplinux_ui_form_set_constraint(%s) = %s", name, result);
	if (!done)
		return;
	constraint_type = type;
	constraint_locked = locked;
	constraint_masked = masked;
	show_toggles(app, form);
}

static void constraint_command(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *form, int command_id,
			       void *context)
{
	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_LOCK:
		apply_constraint(context, form, constraint_type,
				 !constraint_locked, constraint_masked);
		break;
	case COMMAND_MASK:
		apply_constraint(context, form, constraint_type,
				 constraint_locked, !constraint_masked);
		break;
	case COMMAND_ANY:
		apply_constraint(context, form, FPLINUX_UI_TEXT_ANY,
				 constraint_locked, constraint_masked);
		break;
	case COMMAND_NUMERIC:
		apply_constraint(context, form, FPLINUX_UI_TEXT_NUMERIC,
				 constraint_locked, constraint_masked);
		break;
	case COMMAND_DECIMAL:
		apply_constraint(context, form, FPLINUX_UI_TEXT_DECIMAL,
				 constraint_locked, constraint_masked);
		break;
	case COMMAND_PHONE:
		apply_constraint(context, form, FPLINUX_UI_TEXT_PHONE,
				 constraint_locked, constraint_masked);
		break;
	default:
		break;
	}
}

static void open_constraint(struct apidemo *app,
			    const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	constraint_type = FPLINUX_UI_TEXT_ANY;
	constraint_locked = false;
	constraint_masked = false;
	constraint_status = fplinux_ui_form_append_string(
		form, _("Result"), _("Type letters, then choose a type."));
	constraint_field = fplinux_ui_form_append_text_field(
		form, _("Value"), "2024", 12, FPLINUX_UI_TEXT_ANY);
	show_toggles(app, form);
	add_command(form, COMMAND_ANY, FPLINUX_UI_COMMAND_SCREEN, _("Any text"),
		    3, apidemo_glyph(app, "keyboard"));
	add_command(form, COMMAND_NUMERIC, FPLINUX_UI_COMMAND_SCREEN,
		    _("Integer"), 4, apidemo_glyph(app, "keyboard"));
	add_command(form, COMMAND_DECIMAL, FPLINUX_UI_COMMAND_SCREEN,
		    _("Decimal number"), 5, apidemo_glyph(app, "keyboard"));
	add_command(form, COMMAND_PHONE, FPLINUX_UI_COMMAND_SCREEN,
		    _("Phone number"), 6, apidemo_glyph(app, "call"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, constraint_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Choices */

static const char *const sizes[] = { N_("Small"), N_("Medium"), N_("Large") };
static const char *const toppings[] = { N_("Cheese"), N_("Olives"),
					N_("Mushrooms"), N_("Peppers") };
static const char *const crusts[] = { N_("Thin"), N_("Classic"), N_("Thick") };

#define CHOICES_MAX 4

static size_t choice_status;
static size_t choice_size;
static size_t choice_toppings;
static size_t choice_crust;

/* The runtime copies the choice texts. */
static size_t append_choice(struct fplinux_ui_screen *form, const char *label,
			    enum fplinux_ui_choice_kind kind,
			    const char *const *choices, size_t count)
{
	const char *texts[CHOICES_MAX];

	if (count > CHOICES_MAX) {
		errno = EINVAL;
		return SIZE_MAX;
	}
	for (size_t i = 0; i < count; ++i)
		texts[i] = _(choices[i]);
	return fplinux_ui_form_append_choice(form, label, kind, texts, count);
}

static size_t chosen_toppings(const struct fplinux_ui_screen *form)
{
	size_t count = 0;

	for (size_t i = 0; i < APIDEMO_COUNT(toppings); ++i)
		count += fplinux_ui_form_is_chosen(form, choice_toppings, i);
	return count;
}

/* A choice reports OK on a row, a popup the choice made in its list. */
static void choices_changed(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, size_t index,
			    void *context)
{
	size_t count;

	(void)ui;
	(void)context;
	if (index == choice_size)
		show_status(form, choice_status, _("Item %zu: %s"), index,
			    _(sizes[fplinux_ui_form_get_choice(form, index)]));
	else if (index == choice_crust)
		show_status(form, choice_status, _("Item %zu: %s"), index,
			    _(crusts[fplinux_ui_form_get_choice(form, index)]));
	else if (index == choice_toppings && (count = chosen_toppings(form)))
		show_status(form, choice_status,
			    ngettext("Item %zu: %zu chosen",
				     "Item %zu: %zu chosen", count),
			    index, count);
	else if (index == choice_toppings)
		show_status(form, choice_status, _("Item %zu: none chosen"),
			    index);
}

/* EXCLUSIVE and POPUP: one index; MULTIPLE: one flag per choice. */
static void show_choices(struct apidemo *app, struct fplinux_ui_screen *form)
{
	char text[384] = "";

	append_text(text, sizeof(text), "get_choice(%zu) = %zu\n", choice_size,
		    fplinux_ui_form_get_choice(form, choice_size));
	for (size_t i = 0; i < APIDEMO_COUNT(toppings); ++i)
		append_text(text, sizeof(text), "is_chosen(%zu, %zu) = %s\n",
			    choice_toppings, i,
			    fplinux_ui_form_is_chosen(form, choice_toppings,
						      i) ?
				    "true" :
				    "false");
	append_text(text, sizeof(text), "get_choice(%zu) = %zu", choice_crust,
		    fplinux_ui_form_get_choice(form, choice_crust));
	apidemo_show(app, _("Values"), text);
}

/* Changes by the application do not call the listener. */
static void choose_all(struct fplinux_ui_screen *form, bool chosen)
{
	for (size_t i = 0; i < APIDEMO_COUNT(toppings); ++i)
		fplinux_ui_form_set_chosen(form, choice_toppings, i, chosen);
}

static void choices_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, int command_id,
			    void *context)
{
	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_SHOW:
		show_choices(context, form);
		break;
	case COMMAND_CHOOSE_ALL:
		choose_all(form, true);
		break;
	case COMMAND_RESET:
		fplinux_ui_form_set_choice(form, choice_size, 0);
		choose_all(form, false);
		fplinux_ui_form_set_choice(form, choice_crust, 0);
		break;
	default:
		break;
	}
}

/*
 * EXCLUSIVE shows radio rows, MULTIPLE check rows; a POPUP choice is one
 * line with the current choice, and OK opens the list of choices.
 */
static void open_choices(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	choice_status = append_status(form);
	choice_size = append_choice(form, _("Size"),
				    FPLINUX_UI_CHOICE_EXCLUSIVE, sizes,
				    APIDEMO_COUNT(sizes));
	choice_toppings = append_choice(form, _("Toppings"),
					FPLINUX_UI_CHOICE_MULTIPLE, toppings,
					APIDEMO_COUNT(toppings));
	choice_crust = append_choice(form, _("Crust"), FPLINUX_UI_CHOICE_POPUP,
				     crusts, APIDEMO_COUNT(crusts));
	/* The application's own choices at the start. */
	fplinux_ui_form_set_choice(form, choice_size, 1);
	fplinux_ui_form_set_chosen(form, choice_toppings, 0, true);
	fplinux_ui_form_set_choice(form, choice_crust, 1);
	add_command(form, COMMAND_SHOW, FPLINUX_UI_COMMAND_SCREEN,
		    _("Show values"), 1, apidemo_glyph(app, "view"));
	add_command(form, COMMAND_CHOOSE_ALL, FPLINUX_UI_COMMAND_SCREEN,
		    _("Choose all toppings"), 2, apidemo_glyph(app, "check"));
	add_command(form, COMMAND_RESET, FPLINUX_UI_COMMAND_SCREEN, _("Reset"),
		    3, apidemo_glyph(app, "refresh"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, choices_command, choices_changed,
				       app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Gauges */

/* The progress bar fills in five seconds while its timer runs. */
#define PROGRESS_MAXIMUM 100
#define PROGRESS_STEP 2
#define PROGRESS_STEP_MS 100

static struct fplinux_ui_screen *gauges_form;
static size_t gauges_status;
static size_t gauge_small;
static size_t gauge_large;
static size_t gauge_progress;
static lv_timer_t *progress_timer;

static void stop_progress(void)
{
	if (progress_timer)
		lv_timer_delete(progress_timer);
	progress_timer = NULL;
}

/* OK starts or stops the progress bar; the label says which. */
static void show_start(struct apidemo *app, struct fplinux_ui_screen *form)
{
	add_command(form, COMMAND_START, FPLINUX_UI_COMMAND_OK,
		    progress_timer ? _("Stop") : _("Start"), 0,
		    apidemo_glyph(app, progress_timer ? "stop" : "play"));
}

/* The bar stops at its maximum; the timer goes with it. */
static void progress_step(lv_timer_t *timer)
{
	struct apidemo *app = lv_timer_get_user_data(timer);
	int value = fplinux_ui_form_get_value(gauges_form, gauge_progress);

	fplinux_ui_form_set_value(gauges_form, gauge_progress,
				  value + PROGRESS_STEP);
	if (value + PROGRESS_STEP < PROGRESS_MAXIMUM)
		return;
	stop_progress();
	show_start(app, gauges_form);
}

static void toggle_progress(struct apidemo *app, struct fplinux_ui_screen *form)
{
	if (progress_timer) {
		stop_progress();
	} else {
		if (fplinux_ui_form_get_value(form, gauge_progress) >=
		    PROGRESS_MAXIMUM)
			fplinux_ui_form_set_value(form, gauge_progress, 0);
		progress_timer =
			lv_timer_create(progress_step, PROGRESS_STEP_MS, app);
	}
	show_start(app, form);
}

/* An interactive gauge reports every step of Left and Right. */
static void gauges_changed(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *form, size_t index,
			   void *context)
{
	(void)ui;
	(void)context;
	show_status(form, gauges_status, _("Item %zu: %d"), index,
		    fplinux_ui_form_get_value(form, index));
}

static void gauges_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *form, int command_id,
			   void *context)
{
	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		/* The timer must not outlive the screen. */
		stop_progress();
		gauges_form = NULL;
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_START:
		toggle_progress(context, form);
		break;
	case COMMAND_SHOW:
		apidemo_showf(context, _("Values"),
			      "get_value(%zu) = %d\nget_value(%zu) = %d\n"
			      "get_value(%zu) = %d",
			      gauge_small,
			      fplinux_ui_form_get_value(form, gauge_small),
			      gauge_large,
			      fplinux_ui_form_get_value(form, gauge_large),
			      gauge_progress,
			      fplinux_ui_form_get_value(form, gauge_progress));
		break;
	case COMMAND_RESET:
		fplinux_ui_form_set_value(form, gauge_small, 5);
		fplinux_ui_form_set_value(form, gauge_large, 500);
		fplinux_ui_form_set_value(form, gauge_progress, 0);
		break;
	default:
		break;
	}
}

static void open_gauges(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	gauges_form = form;
	gauges_status = append_status(form);
	/* A range of up to 20 moves by 1, a longer one by a twentieth. */
	gauge_small = fplinux_ui_form_append_gauge(form, _("0–10, step 1"), 0,
						   10, 5, true);
	gauge_large = fplinux_ui_form_append_gauge(form, _("0–1000, step 50"),
						   0, 1000, 500, true);
	/* Not interactive: a progress bar that the application moves. */
	gauge_progress = fplinux_ui_form_append_gauge(
		form, _("Progress"), 0, PROGRESS_MAXIMUM, 0, false);
	show_start(app, form);
	add_command(form, COMMAND_SHOW, FPLINUX_UI_COMMAND_SCREEN,
		    _("Show values"), 1, apidemo_glyph(app, "view"));
	add_command(form, COMMAND_RESET, FPLINUX_UI_COMMAND_SCREEN, _("Reset"),
		    2, apidemo_glyph(app, "refresh"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, gauges_command, gauges_changed,
				       app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Dates and times */

static size_t dates_status;
static size_t date_items[3];
static size_t duration_item;

/* 30 August 2026, 14:05 local time: the same value on every run. */
static time_t sample_time(void)
{
	struct tm when = {
		.tm_year = 2026 - 1900,
		.tm_mon = 7,
		.tm_mday = 30,
		.tm_hour = 14,
		.tm_min = 5,
		.tm_isdst = -1,
	};

	return mktime(&when);
}

/* A date item reports when its edit is done with OK. */
static void dates_changed(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  size_t index, void *context)
{
	char date[64];
	char clock[16];

	(void)context;
	if (index == duration_item) {
		show_status(form, dates_status, _("Duration: %u s"),
			    fplinux_ui_form_get_duration(form, duration_item));
		return;
	}
	if (!fplinux_ui_format_date(ui, fplinux_ui_form_get_date(form, index),
				    FPLINUX_UI_DATE_LONG, date, sizeof(date)) ||
	    !fplinux_ui_format_date(ui, fplinux_ui_form_get_date(form, index),
				    FPLINUX_UI_DATE_CLOCK, clock,
				    sizeof(clock)))
		return;
	show_status(form, dates_status, _("Item %zu: %s, %s"), index, date,
		    clock);
}

static void dates_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	char text[256] = "";
	time_t now = time(NULL);

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_NOW:
		for (size_t i = 0; i < APIDEMO_COUNT(date_items); ++i)
			fplinux_ui_form_set_date(form, date_items[i], now);
		break;
	case COMMAND_RESET:
		fplinux_ui_form_set_duration(form, duration_item, 90);
		break;
	case COMMAND_SHOW:
		/* Calendar values and elapsed seconds have separate APIs. */
		for (size_t i = 0; i < APIDEMO_COUNT(date_items); ++i)
			append_text(text, sizeof(text),
				    "get_date(%zu) =\n%lld%s", date_items[i],
				    (long long)fplinux_ui_form_get_date(
					    form, date_items[i]),
				    i + 1 < APIDEMO_COUNT(date_items) ? "\n" :
									"");
		append_text(text, sizeof(text), "\nDuration: %u s",
			    fplinux_ui_form_get_duration(form, duration_item));
		apidemo_show(context, _("Values"), text);
		break;
	default:
		break;
	}
}

static void open_dates(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));
	time_t when = sample_time();

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	dates_status = append_status(form);
	/* Shown in the system language; OK edits the parts in that order. */
	date_items[0] = fplinux_ui_form_append_date(form, _("Date"),
						    FPLINUX_UI_DATE, when);
	date_items[1] = fplinux_ui_form_append_date(form, _("Time"),
						    FPLINUX_UI_TIME, when);
	date_items[2] = fplinux_ui_form_append_date(form, _("Date and time"),
						    FPLINUX_UI_DATE_TIME, when);
	duration_item =
		fplinux_ui_form_append_duration(form, _("Duration"), 90);
	add_command(form, COMMAND_RESET, FPLINUX_UI_COMMAND_SCREEN,
		    _("Reset duration"), 3, apidemo_glyph(app, "refresh"));
	add_command(form, COMMAND_NOW, FPLINUX_UI_COMMAND_SCREEN, _("Now"), 1,
		    apidemo_glyph(app, "date-time"));
	add_command(form, COMMAND_SHOW, FPLINUX_UI_COMMAND_SCREEN,
		    _("Show values"), 2, apidemo_glyph(app, "view"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, dates_command, dates_changed, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Images */

static void open_images(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	/* Scaled down to the form's width and visible height, never up. */
	fplinux_ui_form_append_image(form, _("A colour photo"), APIDEMO_PHOTO);
	fplinux_ui_form_append_image(form, NULL, APIDEMO_PHOTO);
	/* Alpha-only images are drawn in the form's text colour. */
	fplinux_ui_form_append_image(form, _("An alpha-only shape"),
				     APIDEMO_SHAPE);
	fplinux_ui_form_append_image(form, _("A system glyph"),
				     apidemo_glyph(app, "about"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, apidemo_back_listener, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Changing items */

/*
 * Each item is a read-only text field, which takes the focus and so can
 * carry item commands: Move up, Move down, Hide and Delete act on the
 * focused item. The form has no call that counts its items: the
 * application counts them.
 */
static size_t item_count;
static int items_added;

static void add_item_commands(struct apidemo *app,
			      struct fplinux_ui_screen *form, size_t item)
{
	const struct fplinux_ui_command commands[] = {
		{ .id = COMMAND_MOVE_UP,
		  .type = FPLINUX_UI_COMMAND_ITEM,
		  .label = _("Move up"),
		  .priority = 1,
		  .icon = apidemo_glyph(app, "move-up") },
		{ .id = COMMAND_MOVE_DOWN,
		  .type = FPLINUX_UI_COMMAND_ITEM,
		  .label = _("Move down"),
		  .priority = 2,
		  .icon = apidemo_glyph(app, "move-down") },
		{ .id = COMMAND_HIDE,
		  .type = FPLINUX_UI_COMMAND_ITEM,
		  .label = _("Hide"),
		  .priority = 3,
		  .icon = apidemo_glyph(app, "hide") },
		{ .id = COMMAND_DELETE,
		  .type = FPLINUX_UI_COMMAND_ITEM,
		  .label = _("Delete"),
		  .priority = 4,
		  .icon = apidemo_glyph(app, "delete") },
	};

	for (size_t i = 0; i < APIDEMO_COUNT(commands); ++i)
		fplinux_ui_form_add_item_command(form, item, &commands[i]);
}

static size_t append_item(struct apidemo *app, struct fplinux_ui_screen *form)
{
	char text[32];
	size_t item;

	snprintf(text, sizeof(text), _("Item %d"), ++items_added);
	item = fplinux_ui_form_append_text_field(
		form, NULL, text, 0,
		FPLINUX_UI_TEXT_ANY | FPLINUX_UI_TEXT_UNEDITABLE);
	if (item == SIZE_MAX)
		return SIZE_MAX;
	++item_count;
	add_item_commands(app, form, item);
	return item;
}

/* Indices, focus and item commands follow every move and deletion. */
static void items_item(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
		       size_t index, int command_id, void *context)
{
	(void)ui;
	(void)context;
	switch (command_id) {
	case COMMAND_MOVE_UP:
		if (index > 0)
			fplinux_ui_form_move_item(form, index, index - 1);
		break;
	case COMMAND_MOVE_DOWN:
		if (index + 1 < item_count)
			fplinux_ui_form_move_item(form, index, index + 1);
		break;
	case COMMAND_HIDE:
		/* A hidden item keeps its index and passes the focus on. */
		fplinux_ui_form_set_hidden(form, index, true);
		break;
	case COMMAND_DELETE:
		if (fplinux_ui_form_delete(form, index))
			--item_count;
		break;
	default:
		break;
	}
}

static void list_items(struct apidemo *app, struct fplinux_ui_screen *form)
{
	char text[512] = "";

	if (!item_count) {
		apidemo_show(app, _("Items"), _("The form is empty."));
		return;
	}
	for (size_t i = 0; i < item_count; ++i) {
		const char *name = fplinux_ui_form_get_text(form, i);

		append_text(text, sizeof(text),
			    fplinux_ui_form_is_hidden(form, i) ?
				    _("%zu: %s, hidden") :
				    _("%zu: %s"),
			    i, name ? name : "");
		append_text(text, sizeof(text), "%s",
			    i + 1 < item_count ? "\n" : "");
	}
	apidemo_show(app, _("Items"), text);
}

static void items_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	struct apidemo *app = context;
	size_t item;

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_INSERT:
		/* To insert, append and then move the item into place. */
		item = append_item(app, form);
		if (item != SIZE_MAX) {
			fplinux_ui_form_move_item(form, item, 0);
			fplinux_ui_form_set_focus(form, 0);
		}
		break;
	case COMMAND_SHOW_HIDDEN:
		for (size_t i = 0; i < item_count; ++i)
			if (fplinux_ui_form_is_hidden(form, i))
				fplinux_ui_form_set_hidden(form, i, false);
		break;
	case COMMAND_FOCUS_LAST:
		/* A hidden item cannot take the focus: nothing happens. */
		if (item_count)
			fplinux_ui_form_set_focus(form, item_count - 1);
		break;
	case COMMAND_LIST_ITEMS:
		list_items(app, form);
		break;
	case COMMAND_DELETE_ALL:
		/* An empty form has no focus. */
		fplinux_ui_form_delete_all(form);
		item_count = 0;
		break;
	default:
		break;
	}
}

static void open_items(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	item_count = 0;
	items_added = 0;
	for (int i = 0; i < 4; ++i)
		append_item(app, form);
	add_command(form, COMMAND_INSERT, FPLINUX_UI_COMMAND_SCREEN,
		    _("Insert at the top"), 5, apidemo_glyph(app, "add"));
	add_command(form, COMMAND_SHOW_HIDDEN, FPLINUX_UI_COMMAND_SCREEN,
		    _("Show hidden"), 6, apidemo_glyph(app, "view"));
	add_command(form, COMMAND_FOCUS_LAST, FPLINUX_UI_COMMAND_SCREEN,
		    _("Focus the last"), 7, apidemo_glyph(app, "move-down"));
	add_command(form, COMMAND_LIST_ITEMS, FPLINUX_UI_COMMAND_SCREEN,
		    _("List the items"), 8, apidemo_glyph(app, "list"));
	add_command(form, COMMAND_DELETE_ALL, FPLINUX_UI_COMMAND_SCREEN,
		    _("Delete all"), 10, apidemo_glyph(app, "delete"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, items_command, NULL, app);
	fplinux_ui_screen_set_item_listener(form, items_item, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Item commands */

/*
 * A contact form whose items carry their own commands, offered while the
 * item has the focus. The read-only phone number has an OK item command,
 * Call; the screen command Calls off removes it again.
 */
static const char *const ringtones[] = { N_("Bells"), N_("Chime"),
					 N_("Marimba") };

static size_t contact_phone;
static size_t contact_birthday;
static size_t contact_volume;
static size_t contact_ringtone;
static bool contact_calls;

static const struct fplinux_ui_command contact_call = {
	.id = COMMAND_CALL,
	.type = FPLINUX_UI_COMMAND_OK,
	.label = N_("Call"),
	.priority = 0,
};

/* Labels are translated on use; the runtime copies them. */
static bool add_call(struct apidemo *app, struct fplinux_ui_screen *form)
{
	struct fplinux_ui_command call = contact_call;

	call.label = _(contact_call.label);
	call.icon = apidemo_glyph(app, "call");
	return fplinux_ui_form_add_item_command(form, contact_phone, &call);
}

static void show_calls(struct apidemo *app, struct fplinux_ui_screen *form)
{
	add_command(form, COMMAND_CALLS, FPLINUX_UI_COMMAND_SCREEN,
		    contact_calls ? _("Calls off") : _("Calls on"), 1,
		    apidemo_glyph(app, contact_calls ? "cancel" : "call"));
}

/* Actions attached to the contact's fields. */
static void contact_action(struct apidemo *app, struct fplinux_ui_screen *form,
			   size_t item, int command_id)
{
	const char *number = fplinux_ui_form_get_text(form, contact_phone);
	size_t ringtone;

	switch (command_id) {
	case COMMAND_SEND:
		apidemo_showf(app, _("Message"), _("A message to %s."),
			      number ? number : "");
		break;
	case COMMAND_TODAY:
		fplinux_ui_form_set_date(form, item, time(NULL));
		break;
	case COMMAND_MUTE:
		fplinux_ui_form_set_value(form, item, 0);
		break;
	case COMMAND_PLAY:
		ringtone = fplinux_ui_form_get_choice(form, item);
		apidemo_showf(app, _("Ringtone"), _("Playing %s."),
			      ringtone < APIDEMO_COUNT(ringtones) ?
				      _(ringtones[ringtone]) :
				      "");
		break;
	default:
		break;
	}
}

/* With an item listener, item commands arrive here with their item. */
static void contact_item(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			 size_t index, int command_id, void *context)
{
	const char *number = fplinux_ui_form_get_text(form, index);

	(void)ui;
	if (command_id == COMMAND_CALL)
		apidemo_showf(context, _("Call"),
			      _("Calling %s.\nThe item listener received "
				"item %zu."),
			      number ? number : "", index);
	else
		contact_action(context, form, index, command_id);
}

/* Calls on and off: an item command can be removed and added again. */
static void toggle_calls(struct apidemo *app, struct fplinux_ui_screen *form)
{
	if (contact_calls)
		fplinux_ui_screen_remove_item_command(form, contact_phone,
						      COMMAND_CALL);
	else
		add_call(app, form);
	contact_calls = !contact_calls;
	show_calls(app, form);
}

static void contact_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, int command_id,
			    void *context)
{
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_CALLS)
		toggle_calls(context, form);
}

static void open_item_commands(struct apidemo *app,
			       const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command send = {
		.id = COMMAND_SEND,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = _("Send a message"),
		.priority = 1,
		.icon = apidemo_glyph(app, "send"),
	};
	const struct fplinux_ui_command today = {
		.id = COMMAND_TODAY,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = _("Today"),
		.priority = 1,
		.icon = apidemo_glyph(app, "date-time"),
	};
	const struct fplinux_ui_command mute = {
		.id = COMMAND_MUTE,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = _("Mute"),
		.priority = 1,
		.icon = apidemo_glyph(app, "mute"),
	};
	const struct fplinux_ui_command play = {
		.id = COMMAND_PLAY,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = _("Play"),
		.priority = 1,
		.icon = apidemo_glyph(app, "play"),
	};
	const char *ringtone_texts[APIDEMO_COUNT(ringtones)];
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(ringtones); ++i)
		ringtone_texts[i] = _(ringtones[i]);
	contact_phone = fplinux_ui_form_append_text_field(
		form, _("Phone number"), "+15551234567", 0,
		FPLINUX_UI_TEXT_PHONE | FPLINUX_UI_TEXT_UNEDITABLE);
	contact_birthday = fplinux_ui_form_append_date(
		form, _("Birthday"), FPLINUX_UI_DATE, sample_time());
	contact_volume =
		fplinux_ui_form_append_gauge(form, _("Volume"), 0, 10, 7, true);
	contact_ringtone = fplinux_ui_form_append_choice(
		form, _("Ringtone"), FPLINUX_UI_CHOICE_POPUP, ringtone_texts,
		APIDEMO_COUNT(ringtone_texts));
	contact_calls = add_call(app, form);
	fplinux_ui_form_add_item_command(form, contact_phone, &send);
	/* A date's own OK is Edit: its item command goes to the left key. */
	fplinux_ui_form_add_item_command(form, contact_birthday, &today);
	fplinux_ui_form_add_item_command(form, contact_volume, &mute);
	fplinux_ui_form_add_item_command(form, contact_ringtone, &play);
	show_calls(app, form);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, contact_command, NULL, app);
	fplinux_ui_screen_set_item_listener(form, contact_item, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Links and buttons */

/*
 * MIDP's appearance modes. A string or image item drawn as a HYPERLINK or a
 * BUTTON takes the focus while it has item commands: OK runs the first of
 * them by priority and the soft keys offer the others. Without commands it
 * looks and acts as PLAIN, and a PLAIN item never takes the focus, even
 * with item commands.
 */
static size_t links_status;
static size_t links_button;
static enum fplinux_ui_appearance links_button_appearance;
static size_t links_waiting;
static bool links_armed;

static const char *appearance_name(enum fplinux_ui_appearance appearance)
{
	switch (appearance) {
	case FPLINUX_UI_APPEARANCE_PLAIN:
		return "PLAIN";
	case FPLINUX_UI_APPEARANCE_HYPERLINK:
		return "HYPERLINK";
	case FPLINUX_UI_APPEARANCE_BUTTON:
		return "BUTTON";
	default:
		return "?";
	}
}

/* An item command's own label, translated on use; the runtime copies it. */
static bool add_item_command(struct apidemo *app,
			     struct fplinux_ui_screen *form, size_t item,
			     int id, const char *label, int priority,
			     const char *glyph)
{
	const struct fplinux_ui_command command = {
		.id = id,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = label,
		.priority = priority,
		.icon = apidemo_glyph(app, glyph),
	};

	return fplinux_ui_form_add_item_command(form, item, &command);
}

/* SIZE_MAX, no focus, as its name; other indices as numbers. */
static void describe_index(char *text, size_t size, size_t index)
{
	if (index == SIZE_MAX)
		snprintf(text, size, "SIZE_MAX");
	else
		snprintf(text, size, "%zu", index);
}

/*
 * OK ran the item's first command, or Options one of the others; the
 * readout keeps the last one after the alert closes.
 */
static void links_item(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
		       size_t index, int command_id, void *context)
{
	const char *name;
	char focus[24];

	(void)ui;
	switch (command_id) {
	case COMMAND_OPEN:
		name = _("Open");
		break;
	case COMMAND_COPY:
		name = _("Copy the address");
		break;
	case COMMAND_CALL:
		name = _("Call");
		break;
	case COMMAND_VIEW:
		name = _("View");
		break;
	default:
		return;
	}
	describe_index(focus, sizeof(focus), fplinux_ui_form_get_focus(form));
	show_status(form, links_status,
		    _("Item %zu ran “%s”.\nget_focus() = %s"), index, name,
		    focus);
	apidemo_showf(context, name, _("Item %zu ran “%s”.\nget_focus() = %s"),
		      index, name, focus);
}

/* The button item turns PLAIN, then HYPERLINK, then BUTTON again. */
static void change_button(struct fplinux_ui_screen *form)
{
	static const enum fplinux_ui_appearance next[] = {
		[FPLINUX_UI_APPEARANCE_PLAIN] = FPLINUX_UI_APPEARANCE_HYPERLINK,
		[FPLINUX_UI_APPEARANCE_HYPERLINK] =
			FPLINUX_UI_APPEARANCE_BUTTON,
		[FPLINUX_UI_APPEARANCE_BUTTON] = FPLINUX_UI_APPEARANCE_PLAIN,
	};
	enum fplinux_ui_appearance appearance = next[links_button_appearance];
	char result[32];
	bool done;

	errno = 0;
	done = fplinux_ui_form_set_appearance(form, links_button, appearance);
	describe_result(result, sizeof(result), done, errno);
	show_status(form, links_status, "set_appearance(%zu, %s) = %s",
		    links_button, appearance_name(appearance), result);
	if (done)
		links_button_appearance = appearance;
}

/*
 * The HYPERLINK item without commands looks plain. Its first command makes
 * it a link that takes the focus; losing its last command while it has the
 * focus passes the focus on, as deleting it would.
 */
static void arm_link(struct apidemo *app, struct fplinux_ui_screen *form)
{
	if (links_armed)
		fplinux_ui_screen_remove_item_command(form, links_waiting,
						      COMMAND_OPEN);
	else
		add_item_command(app, form, links_waiting, COMMAND_OPEN,
				 _("Open"), 1, "next");
	links_armed = !links_armed;
	add_command(form, COMMAND_ARM, FPLINUX_UI_COMMAND_SCREEN,
		    links_armed ? _("Take its command") :
				  _("Give it a command"),
		    5, apidemo_glyph(app, links_armed ? "delete" : "add"));
}

static void links_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	char focus[24];

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_SHOW_FOCUS:
		describe_index(focus, sizeof(focus),
			       fplinux_ui_form_get_focus(form));
		apidemo_showf(context, _("Focus"),
			      "fplinux_ui_form_get_focus()\n= %s", focus);
		break;
	case COMMAND_CHANGE_BUTTON:
		change_button(form);
		break;
	case COMMAND_ARM:
		arm_link(context, form);
		break;
	default:
		break;
	}
}

static void open_links(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));
	size_t item;

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	/* A link with two commands: OK opens, Options offers the copy. */
	item = fplinux_ui_form_append_string(form, _("Website"), "fplinux.org");
	fplinux_ui_form_set_appearance(form, item,
				       FPLINUX_UI_APPEARANCE_HYPERLINK);
	add_item_command(app, form, item, COMMAND_OPEN, _("Open"), 1, "next");
	add_item_command(app, form, item, COMMAND_COPY, _("Copy the address"),
			 2, "copy");
	/* A button for one action, as a native application's form has it. */
	links_button =
		fplinux_ui_form_append_string(form, NULL, _("Call support"));
	links_button_appearance = FPLINUX_UI_APPEARANCE_BUTTON;
	fplinux_ui_form_set_appearance(form, links_button,
				       links_button_appearance);
	add_item_command(app, form, links_button, COMMAND_CALL, _("Call"), 1,
			 "call");
	/* Images take the same appearances; an alpha-only one takes colour. */
	item = fplinux_ui_form_append_image(form, _("Image link"),
					    APIDEMO_SHAPE);
	fplinux_ui_form_set_appearance(form, item,
				       FPLINUX_UI_APPEARANCE_HYPERLINK);
	add_item_command(app, form, item, COMMAND_VIEW, _("View"), 1, "view");
	item = fplinux_ui_form_append_image(form, _("Image button"),
					    APIDEMO_PHOTO);
	fplinux_ui_form_set_appearance(form, item,
				       FPLINUX_UI_APPEARANCE_BUTTON);
	add_item_command(app, form, item, COMMAND_VIEW, _("View"), 1, "view");
	/* PLAIN, the default: read-only, its command never offered. */
	item = fplinux_ui_form_append_string(form, _("PLAIN with a command"),
					     _("It never takes the focus."));
	add_item_command(app, form, item, COMMAND_OPEN, _("Open"), 1, "next");
	links_armed = false;
	links_waiting = fplinux_ui_form_append_string(
		form, _("HYPERLINK without commands"),
		_("It looks plain until it has a command."));
	fplinux_ui_form_set_appearance(form, links_waiting,
				       FPLINUX_UI_APPEARANCE_HYPERLINK);
	/* Last, so that the link is in sight and takes the focus at once. */
	links_status = fplinux_ui_form_append_string(
		form, _("Item listener"),
		_("OK runs the item's first command."));
	add_command(form, COMMAND_SHOW_FOCUS, FPLINUX_UI_COMMAND_SCREEN,
		    _("Show the focus"), 3, apidemo_glyph(app, "view"));
	add_command(form, COMMAND_CHANGE_BUTTON, FPLINUX_UI_COMMAND_SCREEN,
		    _("Change the button"), 4, apidemo_glyph(app, "edit"));
	add_command(form, COMMAND_ARM, FPLINUX_UI_COMMAND_SCREEN,
		    _("Give it a command"), 5, apidemo_glyph(app, "add"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, links_command, NULL, app);
	fplinux_ui_screen_set_item_listener(form, links_item, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Category hooks: the progress timer runs only while it is seen. */

static void forms_visibility(struct apidemo *app, bool visible)
{
	(void)app;
	if (!progress_timer)
		return;
	if (visible)
		lv_timer_resume(progress_timer);
	else
		lv_timer_pause(progress_timer);
}

static void forms_cleanup(struct apidemo *app)
{
	(void)app;
	stop_progress();
	gauges_form = NULL;
}

static const struct apidemo_entry entries[] = {
	{ N_("String items"),
	  "fplinux_ui_form_append_string · fplinux_ui_form_set_string",
	  open_strings, 0, N_("Change labels and explanatory text") },
	{ N_("Text fields"),
	  "fplinux_ui_form_append_text_field · FPLINUX_UI_TEXT_ANY · "
	  "FPLINUX_UI_TEXT_NUMERIC · FPLINUX_UI_TEXT_DECIMAL · FPLINUX_UI_TEXT_PHONE · "
	  "FPLINUX_UI_TEXT_PASSWORD · FPLINUX_UI_TEXT_UNEDITABLE",
	  open_fields, 0, N_("Enter text, numbers, passwords and a PIN") },
	{ N_("Changing the type"), "fplinux_ui_form_set_constraint",
	  open_constraint, 0, N_("Change a field type, mask or editability") },
	{ N_("Choices"),
	  "fplinux_ui_form_append_choice · FPLINUX_UI_CHOICE_EXCLUSIVE · "
	  "FPLINUX_UI_CHOICE_MULTIPLE · FPLINUX_UI_CHOICE_POPUP",
	  open_choices, 0, N_("Choose size, toppings and crust") },
	{ N_("Gauges"),
	  "fplinux_ui_form_append_gauge · fplinux_ui_form_set_value",
	  open_gauges, 0, N_("Adjust a value and run progress") },
	{ N_("Dates and times"),
	  "fplinux_ui_form_append_date · FPLINUX_UI_DATE · FPLINUX_UI_TIME · "
	  "FPLINUX_UI_DATE_TIME · fplinux_ui_form_append_duration · "
	  "fplinux_ui_form_get_duration · fplinux_ui_form_set_duration",
	  open_dates, 0, N_("Edit a date, time and elapsed duration") },
	{ N_("Form images"), "fplinux_ui_form_append_image", open_images, 0,
	  N_("Fit photos, shapes and glyphs in a form") },
	{ N_("Changing items"),
	  "fplinux_ui_form_move_item · fplinux_ui_form_delete · fplinux_ui_form_set_hidden",
	  open_items, 0, N_("Move, hide, insert and delete form items") },
	{ N_("Item commands"),
	  "fplinux_ui_form_add_item_command · fplinux_ui_screen_set_item_listener",
	  open_item_commands, 0, N_("Use actions on a contact form") },
	{ N_("Links and buttons"),
	  "fplinux_ui_form_set_appearance · FPLINUX_UI_APPEARANCE_HYPERLINK · "
	  "FPLINUX_UI_APPEARANCE_BUTTON",
	  open_links, 0, N_("Activate links and change button actions") },
};

const struct apidemo_category apidemo_forms = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = forms_visibility,
	.cleanup = forms_cleanup,
};
