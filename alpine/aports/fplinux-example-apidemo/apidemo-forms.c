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
#include "apidemo-forms.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Adds a command; the runtime copies the label and decodes the icon. */
void apidemo_forms_add_command(struct fplinux_ui_screen *screen, int id,
			       enum fplinux_ui_command_type type,
			       const char *label, int priority,
			       const void *icon)
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
void apidemo_forms_append_text(char *text, size_t size, const char *format, ...)
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
void apidemo_forms_describe_result(char *text, size_t size, bool done,
				   int error)
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
size_t apidemo_forms_append_status(struct fplinux_ui_screen *form)
{
	return fplinux_ui_form_append_string(form, _("Change listener"),
					     _("Nothing changed yet."));
}

/*
 * A longer text makes the status taller; the form keeps the focused item
 * below it in view.
 */
void apidemo_forms_show_status(struct fplinux_ui_screen *form, size_t status,
			       const char *format, ...)
{
	char text[160];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	fplinux_ui_form_set_string(form, status, text);
}

static const struct apidemo_entry entries[] = {
	{ N_("String items"),
	  "fplinux_ui_form_append_string · fplinux_ui_form_set_string",
	  apidemo_forms_open_strings, 0,
	  N_("Change labels and explanatory text") },
	{ N_("Text fields"),
	  "fplinux_ui_form_append_text_field · FPLINUX_UI_TEXT_ANY · "
	  "FPLINUX_UI_TEXT_NUMERIC · FPLINUX_UI_TEXT_DECIMAL · FPLINUX_UI_TEXT_PHONE · "
	  "FPLINUX_UI_TEXT_PASSWORD · FPLINUX_UI_TEXT_UNEDITABLE",
	  apidemo_forms_open_fields, 0,
	  N_("Enter text, numbers, passwords and a PIN") },
	{ N_("Changing the type"), "fplinux_ui_form_set_constraint",
	  apidemo_forms_open_constraint, 0,
	  N_("Change a field type, mask or editability") },
	{ N_("Choices"),
	  "fplinux_ui_form_append_choice · FPLINUX_UI_CHOICE_EXCLUSIVE · "
	  "FPLINUX_UI_CHOICE_MULTIPLE · FPLINUX_UI_CHOICE_POPUP",
	  apidemo_forms_open_choices, 0,
	  N_("Choose size, toppings and crust") },
	{ N_("Gauges"),
	  "fplinux_ui_form_append_gauge · fplinux_ui_form_set_value",
	  apidemo_forms_open_gauges, 0, N_("Adjust a value and run progress") },
	{ N_("Dates and times"),
	  "fplinux_ui_form_append_date · FPLINUX_UI_DATE · FPLINUX_UI_TIME · "
	  "FPLINUX_UI_DATE_TIME · fplinux_ui_form_append_duration · "
	  "fplinux_ui_form_get_duration · fplinux_ui_form_set_duration",
	  apidemo_forms_open_dates, 0,
	  N_("Edit a date, time and elapsed duration") },
	{ N_("Form images"), "fplinux_ui_form_append_image",
	  apidemo_forms_open_images, 0,
	  N_("Fit photos, shapes and glyphs in a form") },
	{ N_("Changing items"),
	  "fplinux_ui_form_move_item · fplinux_ui_form_delete · fplinux_ui_form_set_hidden",
	  apidemo_forms_open_items, 0,
	  N_("Move, hide, insert and delete form items") },
	{ N_("Item commands"),
	  "fplinux_ui_form_add_item_command · fplinux_ui_screen_set_item_listener",
	  apidemo_forms_open_item_commands, 0,
	  N_("Use actions on a contact form") },
	{ N_("Links and buttons"),
	  "fplinux_ui_form_set_appearance · FPLINUX_UI_APPEARANCE_HYPERLINK · "
	  "FPLINUX_UI_APPEARANCE_BUTTON",
	  apidemo_forms_open_links, 0,
	  N_("Activate links and change button actions") },
};

const struct apidemo_category apidemo_forms = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = apidemo_forms_visibility,
	.cleanup = apidemo_forms_cleanup,
};
