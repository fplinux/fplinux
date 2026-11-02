/* SPDX-License-Identifier: 0BSD */
#include "apidemo-forms.h"

#include <errno.h>
#include <stdio.h>

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

void apidemo_forms_open_strings(struct apidemo *app,
				const struct apidemo_entry *entry)
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
	apidemo_forms_add_command(form, COMMAND_CHANGE, FPLINUX_UI_COMMAND_OK,
				  _("Change"), 0, apidemo_glyph(app, "edit"));
	apidemo_forms_add_command(form, COMMAND_REMOVE_TEXT,
				  FPLINUX_UI_COMMAND_SCREEN, _("Remove text"),
				  1, apidemo_glyph(app, "backspace"));
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
	apidemo_forms_show_status(form, fields_status, _("Item %zu: “%s”"),
				  index, text ? text : "");
}

/* A DECIMAL field returns its number with '.', whatever it shows. */
static void show_fields(struct apidemo *app, struct fplinux_ui_screen *form)
{
	char text[512] = "";

	for (size_t i = 0; i < APIDEMO_COUNT(fields); ++i) {
		const char *value =
			fplinux_ui_form_get_text(form, field_items[i]);

		apidemo_forms_append_text(text, sizeof(text), _("%s: “%s”"),
					  _(fields[i].label),
					  value ? value : "");
		apidemo_forms_append_text(text, sizeof(text), "%s",
					  i + 1 < APIDEMO_COUNT(fields) ? "\n" :
									  "");
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

void apidemo_forms_open_fields(struct apidemo *app,
			       const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	fields_status = apidemo_forms_append_status(form);
	for (size_t i = 0; i < APIDEMO_COUNT(fields); ++i)
		field_items[i] = fplinux_ui_form_append_text_field(
			form, _(fields[i].label),
			fields[i].text ? _(fields[i].text) : NULL,
			fields[i].max_chars, fields[i].constraint);
	/* A text field has no OK action of its own: OK runs Show. */
	apidemo_forms_add_command(form, COMMAND_SHOW, FPLINUX_UI_COMMAND_OK,
				  _("Show"), 0, apidemo_glyph(app, "view"));
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
	apidemo_forms_add_command(
		form, COMMAND_LOCK, FPLINUX_UI_COMMAND_SCREEN,
		constraint_locked ? _("Make editable") : _("Make read-only"), 1,
		apidemo_glyph(app, constraint_locked ? "edit" : "lock"));
	apidemo_forms_add_command(
		form, COMMAND_MASK, FPLINUX_UI_COMMAND_SCREEN,
		constraint_masked ? _("Show the text") : _("Hide the text"), 2,
		apidemo_glyph(app, constraint_masked ? "view" : "hide"));
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
	apidemo_forms_describe_result(result, sizeof(result), done, errno);
	constraint_name(constraint, name, sizeof(name));
	apidemo_forms_show_status(form, constraint_status,
				  "fplinux_ui_form_set_constraint(%s) = %s",
				  name, result);
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

void apidemo_forms_open_constraint(struct apidemo *app,
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
	apidemo_forms_add_command(form, COMMAND_ANY, FPLINUX_UI_COMMAND_SCREEN,
				  _("Any text"), 3,
				  apidemo_glyph(app, "keyboard"));
	apidemo_forms_add_command(form, COMMAND_NUMERIC,
				  FPLINUX_UI_COMMAND_SCREEN, _("Integer"), 4,
				  apidemo_glyph(app, "keyboard"));
	apidemo_forms_add_command(form, COMMAND_DECIMAL,
				  FPLINUX_UI_COMMAND_SCREEN,
				  _("Decimal number"), 5,
				  apidemo_glyph(app, "keyboard"));
	apidemo_forms_add_command(form, COMMAND_PHONE,
				  FPLINUX_UI_COMMAND_SCREEN, _("Phone number"),
				  6, apidemo_glyph(app, "call"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, constraint_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}
