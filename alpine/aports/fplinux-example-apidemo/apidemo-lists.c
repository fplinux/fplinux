/* SPDX-License-Identifier: 0BSD */
/* Lists retain their data while another screen covers them. */
#include "apidemo.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Command ids of the demo screens; each screen uses a few of them. */
enum {
	COMMAND_UNMARK = 1,
	COMMAND_COUNT,
	COMMAND_UNMARK_ALL,
	COMMAND_RENAME,
	COMMAND_NO_DETAIL,
	COMMAND_INSERT_FIRST,
	COMMAND_INSERT_HERE,
	COMMAND_APPEND,
	COMMAND_DELETE,
	COMMAND_DELETE_LAST,
	COMMAND_DELETE_ALL,
	COMMAND_SIZE,
	COMMAND_NEW,
	COMMAND_NEW_TEN,
	COMMAND_DELETE_FIRST,
	COMMAND_CALL,
	COMMAND_FORGET,
	COMMAND_ADD_ICONS,
	COMMAND_DETAILS,
};

/* The rows of the small stored lists. */
static const char *const fruits[] = {
	N_("Apple"), N_("Banana"), N_("Cherry"), N_("Grape"),
	N_("Lemon"), N_("Orange"), N_("Pear"),	 N_("Plum"),
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

/* An index in a result text: its number, or SIZE_MAX for none. */
static const char *index_name(size_t index, char *text, size_t size)
{
	if (index == SIZE_MAX)
		return "SIZE_MAX";
	snprintf(text, size, "%zu", index);
	return text;
}

/* The marked row of a stored EXCLUSIVE list, or SIZE_MAX. */
static size_t marked_row(const struct fplinux_ui_screen *list)
{
	for (size_t i = 0; i < fplinux_ui_list_size(list); ++i)
		if (fplinux_ui_list_is_selected(list, i))
			return i;
	return SIZE_MAX;
}

/*
 * OK on a row marks it and reports it, also when it was marked already,
 * so that OK can mean "choose and return".
 */
static void exclusive_changed(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *list, size_t index,
			      void *context)
{
	(void)ui;
	(void)list;
	apidemo_showf(context, _("Marked"),
		      _("Index %zu: %s.\nOK on the marked row is reported "
			"too."),
		      index, _(fruits[index]));
}

/* Unmarking a row of an EXCLUSIVE list is ignored: one row stays marked. */
static void exclusive_command(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *list, int command_id,
			      void *context)
{
	size_t row = marked_row(list);

	if (command_id == APIDEMO_COMMAND_BACK) {
		fplinux_ui_go_back(ui);
	} else if (command_id == COMMAND_UNMARK && row != SIZE_MAX) {
		fplinux_ui_list_set_selected(list, row, false);
		apidemo_showf(context, _("Unmark"),
			      fplinux_ui_list_is_selected(list, row) ?
				      _("Index %zu is still marked: an "
					"EXCLUSIVE list ignores unmarking.") :
				      _("Index %zu is no longer marked."),
			      row);
	}
}

static void open_exclusive(struct apidemo *app,
			   const struct apidemo_entry *entry)
{
	/* The row marked at the start, chosen by the application. */
	const size_t preset = 2;
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_EXCLUSIVE);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(fruits); ++i)
		fplinux_ui_list_append(list, _(fruits[i]), NULL);
	fplinux_ui_list_set_selected(list, preset, true);
	fplinux_ui_list_set_focused(list, preset);
	add_command(list, COMMAND_UNMARK, FPLINUX_UI_COMMAND_SCREEN,
		    _("Unmark"), 1, apidemo_glyph(app, "cancel"));
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, exclusive_command,
				       exclusive_changed, app);
	apidemo_push(app, list, "fplinux_ui_list_new");
}

/* A MULTIPLE row's detail shows what fplinux_ui_list_is_selected() reads. */
static void show_mark(struct fplinux_ui_screen *list, size_t row)
{
	fplinux_ui_list_set(list, row, _(fruits[row]),
			    fplinux_ui_list_is_selected(list, row) ?
				    _("Marked") :
				    _("Not marked"));
}

/* OK has toggled the row's mark already; the listener only reads it. */
static void multiple_changed(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *list, size_t index,
			     void *context)
{
	(void)ui;
	(void)context;
	show_mark(list, index);
}

static void multiple_command(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *list, int command_id,
			     void *context)
{
	size_t size = fplinux_ui_list_size(list);
	size_t marked = 0;

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_COUNT:
		for (size_t i = 0; i < size; ++i)
			marked += fplinux_ui_list_is_selected(list, i);
		apidemo_showf(context, _("Marked rows"),
			      ngettext("%zu row is marked.",
				       "%zu rows are marked.", marked),
			      marked);
		break;
	case COMMAND_UNMARK_ALL:
		/* Changes by the application do not call the listener. */
		for (size_t i = 0; i < size; ++i) {
			fplinux_ui_list_set_selected(list, i, false);
			show_mark(list, i);
		}
		break;
	default:
		break;
	}
}

static void open_multiple(struct apidemo *app,
			  const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_MULTIPLE);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(fruits); ++i) {
		fplinux_ui_list_append(list, _(fruits[i]), NULL);
		/* Two rows start marked. */
		fplinux_ui_list_set_selected(list, i, i == 1 || i == 4);
		show_mark(list, i);
	}
	add_command(list, COMMAND_COUNT, FPLINUX_UI_COMMAND_SCREEN,
		    _("Count marks"), 1, apidemo_glyph(app, "list"));
	add_command(list, COMMAND_UNMARK_ALL, FPLINUX_UI_COMMAND_SCREEN,
		    _("Unmark all"), 2, apidemo_glyph(app, "cancel"));
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, multiple_command, multiple_changed,
				       app);
	apidemo_push(app, list, "fplinux_ui_list_new");
}

/* Rows: details */

/* A settings list: each row shows its current value as its detail. */
struct setting {
	const char *name;
	const char *const *values;
	size_t count;
	/* The system glyph that Add icons gives the row. */
	const char *glyph;
};

static const char *const themes[] = { N_("Light"), N_("Dark") };
static const char *const text_sizes[] = { N_("Small"), N_("Medium"),
					  N_("Large") };
static const char *const ringtones[] = { N_("Bells"), N_("Chime"),
					 N_("Marimba") };
/*
 * Language names are written in their own language: not marked for
 * translation, gettext() returns them as they are.
 */
static const char *const languages[] = { "English", "Русский" };

static const struct setting settings[] = {
	{ N_("Theme"), themes, APIDEMO_COUNT(themes), "theme" },
	{ N_("Text size"), text_sizes, APIDEMO_COUNT(text_sizes), "display" },
	{ N_("Ringtone"), ringtones, APIDEMO_COUNT(ringtones), "alarm" },
	{ N_("Language"), languages, APIDEMO_COUNT(languages), "language" },
	/* At 128 wide it fits the row until the row has an icon box. */
	{ N_("Notification sound"), ringtones, APIDEMO_COUNT(ringtones),
	  "alarm" },
};

static size_t setting_values[APIDEMO_COUNT(settings)];

static const char *setting_value(size_t row)
{
	return _(settings[row].values[setting_values[row]]);
}

static void details_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *list, int command_id,
			    void *context)
{
	struct apidemo *app = context;
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK) {
		fplinux_ui_go_back(ui);
		return;
	}
	if (row >= APIDEMO_COUNT(settings))
		return;
	switch (command_id) {
	case FPLINUX_UI_SELECT_COMMAND:
		/* The next value; fplinux_ui_list_set() replaces both lines. */
		setting_values[row] =
			(setting_values[row] + 1) % settings[row].count;
		fplinux_ui_list_set(list, row, _(settings[row].name),
				    setting_value(row));
		break;
	case COMMAND_RENAME:
		fplinux_ui_list_set(list, row, _("New name"),
				    setting_value(row));
		break;
	case COMMAND_NO_DETAIL:
		/* The row keeps the detail pitch with an empty second line. */
		fplinux_ui_list_set(list, row, _(settings[row].name), NULL);
		break;
	case COMMAND_ADD_ICONS:
		/*
		 * The list is shown: the first icon gives every row an icon
		 * box, and a focused text that no longer fits scrolls.
		 */
		for (size_t i = 0; i < APIDEMO_COUNT(settings); ++i)
			fplinux_ui_list_set_icon(
				list, i, apidemo_glyph(app, settings[i].glyph),
				FPLINUX_UI_LIST_ICON_IMAGE);
		break;
	default:
		break;
	}
}

static void open_details(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_IMPLICIT);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	/* The first detail gives every row the taller row_height_detail. */
	for (size_t i = 0; i < APIDEMO_COUNT(settings); ++i) {
		setting_values[i] = 0;
		fplinux_ui_list_append(list, _(settings[i].name),
				       setting_value(i));
	}
	fplinux_ui_list_set_select_label(list, _("Change"));
	add_command(list, COMMAND_RENAME, FPLINUX_UI_COMMAND_SCREEN,
		    _("Rename"), 1, apidemo_glyph(app, "edit"));
	add_command(list, COMMAND_NO_DETAIL, FPLINUX_UI_COMMAND_SCREEN,
		    _("Remove detail"), 2, apidemo_glyph(app, "delete"));
	add_command(list, COMMAND_ADD_ICONS, FPLINUX_UI_COMMAND_SCREEN,
		    _("Add icons"), 3, apidemo_glyph(app, "image"));
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, details_command, NULL, app);
	apidemo_push(app, list, "fplinux_ui_list_new");
}

/* Rows: inserting and deleting */

/* Numbers the rows that the demo adds. */
static int rows_added;

static void insert_row(struct apidemo *app, struct fplinux_ui_screen *list,
		       size_t index)
{
	char text[32];

	snprintf(text, sizeof(text), _("New row %d"), ++rows_added);
	/*
	 * The focused row stays the same row; the first row of an empty list
	 * takes the focus.
	 */
	if (fplinux_ui_list_insert(list, index, text, NULL) == SIZE_MAX)
		apidemo_error(app, "fplinux_ui_list_insert");
}

static void delete_row(struct apidemo *app, struct fplinux_ui_screen *list,
		       size_t index)
{
	/* The empty list has nothing to delete. */
	if (index >= fplinux_ui_list_size(list))
		return;
	if (!fplinux_ui_list_delete(list, index))
		apidemo_error(app, "fplinux_ui_list_delete");
}

/* What the size and focus calls return; an empty list has no focus. */
static void show_size(struct apidemo *app, struct fplinux_ui_screen *list)
{
	char focus[24];

	apidemo_showf(app, _("Size and focus"),
		      "fplinux_ui_list_size() = %zu\n"
		      "fplinux_ui_list_get_focused() = %s",
		      fplinux_ui_list_size(list),
		      index_name(fplinux_ui_list_get_focused(list), focus,
				 sizeof(focus)));
}

static void rows_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			 int command_id, void *context)
{
	struct apidemo *app = context;
	size_t size = fplinux_ui_list_size(list);
	size_t focus = fplinux_ui_list_get_focused(list);

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case FPLINUX_UI_SELECT_COMMAND:
		apidemo_showf(app, _("Selected"), _("Index %zu, size %zu."),
			      focus, size);
		break;
	case COMMAND_INSERT_FIRST:
		insert_row(app, list, 0);
		break;
	case COMMAND_INSERT_HERE:
		/* Before the focused row; at 0 in an empty list. */
		insert_row(app, list, focus == SIZE_MAX ? 0 : focus);
		break;
	case COMMAND_APPEND:
		/* Inserting at the size appends. */
		insert_row(app, list, size);
		break;
	case COMMAND_DELETE:
		delete_row(app, list, focus);
		break;
	case COMMAND_DELETE_LAST:
		delete_row(app, list, size - 1);
		break;
	case COMMAND_DELETE_ALL:
		fplinux_ui_list_delete_all(list);
		break;
	case COMMAND_SIZE:
		show_size(app, list);
		break;
	default:
		break;
	}
}

static void open_rows(struct apidemo *app, const struct apidemo_entry *entry)
{
	const void *add = apidemo_glyph(app, "add");
	const void *erase = apidemo_glyph(app, "delete");
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_IMPLICIT);
	char text[32];

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	rows_added = 0;
	for (int i = 1; i <= 5; ++i) {
		snprintf(text, sizeof(text), _("Row %d"), i);
		fplinux_ui_list_append(list, text, NULL);
	}
	/* The options menu lists the commands in this order, with icons. */
	add_command(list, COMMAND_INSERT_FIRST, FPLINUX_UI_COMMAND_SCREEN,
		    _("Insert first"), 1, add);
	add_command(list, COMMAND_INSERT_HERE, FPLINUX_UI_COMMAND_SCREEN,
		    _("Insert here"), 2, add);
	add_command(list, COMMAND_APPEND, FPLINUX_UI_COMMAND_SCREEN,
		    _("Append"), 3, add);
	add_command(list, COMMAND_DELETE, FPLINUX_UI_COMMAND_SCREEN,
		    _("Delete"), 4, erase);
	add_command(list, COMMAND_DELETE_LAST, FPLINUX_UI_COMMAND_SCREEN,
		    _("Delete last row"), 5, erase);
	add_command(list, COMMAND_DELETE_ALL, FPLINUX_UI_COMMAND_SCREEN,
		    _("Delete all"), 6, erase);
	add_command(list, COMMAND_SIZE, FPLINUX_UI_COMMAND_SCREEN,
		    _("Size and focus"), 7, apidemo_glyph(app, "about"));
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, rows_command, NULL, app);
	apidemo_push(app, list, "fplinux_ui_list_new");
}

/* Virtual row strings remain alive until the runtime copies them. */
static char virtual_text[64];

/*
 * An inbox whose data grows and shrinks at both ends: row i shows message
 * number newest - i. After changing the data, the application tells the
 * list how many rows were inserted or deleted, and where; the focus stays
 * on the same message.
 */
#define INBOX_START 1000

static size_t inbox_newest;
static size_t inbox_count;

static bool message_row(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			size_t index, struct fplinux_ui_virtual_row *row,
			void *context)
{
	(void)ui;
	(void)list;
	(void)context;
	if (index >= inbox_count)
		return false;
	snprintf(virtual_text, sizeof(virtual_text), _("Message %zu"),
		 inbox_newest - index);
	row->text = virtual_text;
	return true;
}

static void inbox_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			  int command_id, void *context)
{
	/* How many messages arrive with "10 new messages". */
	const size_t ten = 10;

	switch (command_id) {
	case FPLINUX_UI_SELECT_COMMAND:
	case COMMAND_DETAILS: {
		size_t row = command_id == COMMAND_DETAILS ?
				     fplinux_ui_screen_get_command_item(list) :
				     fplinux_ui_list_get_focused(list);
		char message[64];

		if (row >= inbox_count)
			break;
		snprintf(message, sizeof(message), _("Message %zu"),
			 inbox_newest - row);
		apidemo_showf(context, _("Selected"), _("Index %zu: %s."), row,
			      message);
		break;
	}
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_NEW:
		++inbox_newest;
		++inbox_count;
		fplinux_ui_list_rows_inserted(list, 0, 1);
		break;
	case COMMAND_NEW_TEN:
		inbox_newest += ten;
		inbox_count += ten;
		fplinux_ui_list_rows_inserted(list, 0, ten);
		break;
	case COMMAND_DELETE_FIRST:
		if (!inbox_count)
			break;
		--inbox_newest;
		--inbox_count;
		fplinux_ui_list_rows_deleted(list, 0, 1);
		break;
	case COMMAND_DELETE_LAST:
		if (!inbox_count)
			break;
		--inbox_count;
		fplinux_ui_list_rows_deleted(list, inbox_count, 1);
		break;
	default:
		break;
	}
}

static void open_virtual_changes(struct apidemo *app,
				 const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command details = {
		.id = COMMAND_DETAILS,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Details"),
		.priority = 0,
		.icon = apidemo_glyph(app, "about"),
	};
	struct fplinux_ui_screen *list;

	inbox_newest = INBOX_START;
	inbox_count = INBOX_START;
	list = fplinux_ui_list_new_virtual(app->ui, _(entry->title),
					   FPLINUX_UI_LIST_IMPLICIT,
					   inbox_count, message_row, app);
	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new_virtual");
		return;
	}
	add_command(list, COMMAND_NEW, FPLINUX_UI_COMMAND_SCREEN,
		    _("New message"), 1, apidemo_glyph(app, "add"));
	add_command(list, COMMAND_NEW_TEN, FPLINUX_UI_COMMAND_SCREEN,
		    _("10 new messages"), 2, apidemo_glyph(app, "add"));
	add_command(list, COMMAND_DELETE_FIRST, FPLINUX_UI_COMMAND_SCREEN,
		    _("Delete first"), 3, apidemo_glyph(app, "delete"));
	add_command(list, COMMAND_DELETE_LAST, FPLINUX_UI_COMMAND_SCREEN,
		    _("Delete last"), 4, apidemo_glyph(app, "delete"));
	/*
	 * Details belongs to original row 2 (Message 998). The command's
	 * index moves with rows_inserted/deleted; its message stays the same.
	 * An OK item command replaces Select only on that row, not the menu.
	 */
	if (!fplinux_ui_list_add_item_command(list, 2, &details))
		apidemo_error(app, "fplinux_ui_list_add_item_command");
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, inbox_command, NULL, app);
	apidemo_push(app, list, "fplinux_ui_list_new_virtual");
}

/* Item commands */

/*
 * A contact list whose rows carry their own commands. A row with a number
 * has an OK item command, Call, which replaces Select on that row; every
 * row has Delete, and a row with a number has Forget number, which removes
 * its Call again.
 */
struct contact {
	const char *name;
	/* NULL: no number, and OK selects the row. */
	const char *number;
};

static const struct contact contacts[] = {
	{ N_("Anna"), "+1 555 0101" }, { N_("Boris"), "+1 555 0102" },
	{ N_("Clara"), NULL },	       { N_("Dmitry"), "+1 555 0104" },
	{ N_("Eva"), NULL },
};

/* The contact shown by each row; deleting a row moves the ones after it. */
static size_t contact_rows[APIDEMO_COUNT(contacts)];
static size_t contact_count;

static const char *contact_name(size_t row)
{
	return _(contacts[contact_rows[row]].name);
}

/* What a row's item commands do, whichever listener receives them. */
static void contact_action(struct fplinux_ui_screen *list, size_t row,
			   int command_id)
{
	switch (command_id) {
	case COMMAND_DELETE:
		/* Item commands move with their rows. */
		if (!fplinux_ui_list_delete(list, row))
			break;
		--contact_count;
		memmove(&contact_rows[row], &contact_rows[row + 1],
			(contact_count - row) * sizeof(*contact_rows));
		break;
	case COMMAND_FORGET:
		/* Without its OK item command the row shows Select again. */
		fplinux_ui_screen_remove_item_command(list, row, COMMAND_CALL);
		fplinux_ui_screen_remove_item_command(list, row,
						      COMMAND_FORGET);
		fplinux_ui_list_set(list, row, contact_name(row),
				    _("No number"));
		break;
	default:
		break;
	}
}

/* With an item listener, item commands arrive here with their row. */
static void contact_item(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			 size_t index, int command_id, void *context)
{
	(void)ui;
	if (command_id == COMMAND_CALL)
		apidemo_showf(context, _("Call"),
			      _("Calling %s.\nThe item listener received "
				"index %zu."),
			      contact_name(index), index);
	else
		contact_action(list, index, command_id);
}

/* Screen commands, and Select on a row without its own OK command. */
static void contact_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *list, int command_id,
			    void *context)
{
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND && row != SIZE_MAX)
		apidemo_showf(context, _("Selected"),
			      _("%s has no number to call."),
			      contact_name(row));
}

static void open_item_commands(struct apidemo *app,
			       const struct apidemo_entry *entry)
{
	/* Call is an OK command: on a row of an IMPLICIT list it takes OK. */
	const struct fplinux_ui_command call = {
		.id = COMMAND_CALL,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Call"),
		.priority = 0,
		.icon = apidemo_glyph(app, "call"),
	};
	const struct fplinux_ui_command erase = {
		.id = COMMAND_DELETE,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = _("Delete"),
		.priority = 1,
		.icon = apidemo_glyph(app, "delete"),
	};
	const struct fplinux_ui_command forget = {
		.id = COMMAND_FORGET,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = _("Forget number"),
		.priority = 2,
		.icon = apidemo_glyph(app, "backspace"),
	};
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_IMPLICIT);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	contact_count = APIDEMO_COUNT(contacts);
	for (size_t i = 0; i < contact_count; ++i) {
		const struct contact *contact = &contacts[i];
		size_t row = fplinux_ui_list_append(
			list, _(contact->name),
			contact->number ? contact->number : _("No number"));

		if (row == SIZE_MAX)
			break;
		contact_rows[row] = i;
		fplinux_ui_list_add_item_command(list, row, &erase);
		if (!contact->number)
			continue;
		fplinux_ui_list_add_item_command(list, row, &call);
		fplinux_ui_list_add_item_command(list, row, &forget);
	}
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, contact_command, NULL, app);
	fplinux_ui_screen_set_item_listener(list, contact_item, app);
	apidemo_push(app, list, "fplinux_ui_list_new");
}

static const struct apidemo_entry entries[] = {
	{ N_("List actions"),
	  "fplinux_ui_list_new · FPLINUX_UI_LIST_IMPLICIT · "
	  "fplinux_ui_list_set · fplinux_ui_list_set_select_label · fplinux_ui_list_set_icon",
	  open_details, 0, N_("Change row values, details and icons") },
	{ N_("Exclusive list"),
	  "FPLINUX_UI_LIST_EXCLUSIVE · fplinux_ui_list_set_selected · fplinux_ui_list_is_selected",
	  open_exclusive, 0, N_("Choose one row") },
	{ N_("Multiple list"),
	  "FPLINUX_UI_LIST_MULTIPLE · fplinux_ui_list_is_selected",
	  open_multiple, 0, N_("Mark rows and count the selection") },
	{ N_("Changing rows"),
	  "fplinux_ui_list_insert · fplinux_ui_list_delete · fplinux_ui_list_delete_all",
	  open_rows, 0, N_("Insert and delete without losing focus") },
	{ N_("Virtual list"),
	  "fplinux_ui_list_new_virtual · FPLINUX_UI_VIRTUAL_ROW_HAS · "
	  "fplinux_ui_list_rows_inserted · fplinux_ui_list_rows_deleted",
	  open_virtual_changes, 0, N_("Browse and change a large inbox") },
	{ N_("Item commands"),
	  "fplinux_ui_list_add_item_command · fplinux_ui_screen_set_item_listener · "
	  "fplinux_ui_screen_remove_item_command",
	  open_item_commands, 0, N_("Use actions belonging to each contact") },
};

const struct apidemo_category apidemo_lists = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
};
