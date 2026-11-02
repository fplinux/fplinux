/* SPDX-License-Identifier: 0BSD */
#include "apidemo-forms.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Images */

void apidemo_forms_open_images(struct apidemo *app,
			       const struct apidemo_entry *entry)
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

		apidemo_forms_append_text(text, sizeof(text),
					  fplinux_ui_form_is_hidden(form, i) ?
						  _("%zu: %s, hidden") :
						  _("%zu: %s"),
					  i, name ? name : "");
		apidemo_forms_append_text(text, sizeof(text), "%s",
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

void apidemo_forms_open_items(struct apidemo *app,
			      const struct apidemo_entry *entry)
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
	apidemo_forms_add_command(form, COMMAND_INSERT,
				  FPLINUX_UI_COMMAND_SCREEN,
				  _("Insert at the top"), 5,
				  apidemo_glyph(app, "add"));
	apidemo_forms_add_command(form, COMMAND_SHOW_HIDDEN,
				  FPLINUX_UI_COMMAND_SCREEN, _("Show hidden"),
				  6, apidemo_glyph(app, "view"));
	apidemo_forms_add_command(form, COMMAND_FOCUS_LAST,
				  FPLINUX_UI_COMMAND_SCREEN,
				  _("Focus the last"), 7,
				  apidemo_glyph(app, "move-down"));
	apidemo_forms_add_command(form, COMMAND_LIST_ITEMS,
				  FPLINUX_UI_COMMAND_SCREEN,
				  _("List the items"), 8,
				  apidemo_glyph(app, "list"));
	apidemo_forms_add_command(form, COMMAND_DELETE_ALL,
				  FPLINUX_UI_COMMAND_SCREEN, _("Delete all"),
				  10, apidemo_glyph(app, "delete"));
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
	apidemo_forms_add_command(
		form, COMMAND_CALLS, FPLINUX_UI_COMMAND_SCREEN,
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

void apidemo_forms_open_item_commands(struct apidemo *app,
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
		form, _("Birthday"), FPLINUX_UI_DATE,
		apidemo_forms_sample_time());
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
