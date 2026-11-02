/* SPDX-License-Identifier: 0BSD */
#include "apidemo-forms.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

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
	apidemo_forms_show_status(form, links_status,
				  _("Item %zu ran “%s”.\nget_focus() = %s"),
				  index, name, focus);
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
	apidemo_forms_describe_result(result, sizeof(result), done, errno);
	apidemo_forms_show_status(form, links_status,
				  "set_appearance(%zu, %s) = %s", links_button,
				  appearance_name(appearance), result);
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
	apidemo_forms_add_command(
		form, COMMAND_ARM, FPLINUX_UI_COMMAND_SCREEN,
		links_armed ? _("Take its command") : _("Give it a command"), 5,
		apidemo_glyph(app, links_armed ? "delete" : "add"));
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

void apidemo_forms_open_links(struct apidemo *app,
			      const struct apidemo_entry *entry)
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
	apidemo_forms_add_command(form, COMMAND_SHOW_FOCUS,
				  FPLINUX_UI_COMMAND_SCREEN,
				  _("Show the focus"), 3,
				  apidemo_glyph(app, "view"));
	apidemo_forms_add_command(form, COMMAND_CHANGE_BUTTON,
				  FPLINUX_UI_COMMAND_SCREEN,
				  _("Change the button"), 4,
				  apidemo_glyph(app, "edit"));
	apidemo_forms_add_command(form, COMMAND_ARM, FPLINUX_UI_COMMAND_SCREEN,
				  _("Give it a command"), 5,
				  apidemo_glyph(app, "add"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, links_command, NULL, app);
	fplinux_ui_screen_set_item_listener(form, links_item, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}
