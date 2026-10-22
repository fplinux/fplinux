/* SPDX-License-Identifier: 0BSD */
/*
 * Lists: the List screen of fplinux-ui-screen.h in every variant.
 *
 * Each demo is one open function that builds a list, adds its commands and
 * listeners and pushes it, followed by those listeners. One demo runs at a
 * time, so the little state a demo needs is kept in static variables next
 * to it. The demos come in four groups:
 *
 *   kinds     IMPLICIT, EXCLUSIVE and MULTIPLE lists and the select label;
 *   rows      details, icons, inserting and deleting rows, paging;
 *   virtual   lists whose rows stay in the application's own data;
 *   commands  item commands, with and without an item listener.
 */
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
	COMMAND_REMOVE_ICON,
	COMMAND_INSERT_FIRST,
	COMMAND_INSERT_HERE,
	COMMAND_APPEND,
	COMMAND_DELETE,
	COMMAND_DELETE_LAST,
	COMMAND_DELETE_ALL,
	COMMAND_SIZE,
	COMMAND_OUT_OF_RANGE,
	COMMAND_FIRST_ROW,
	COMMAND_MIDDLE_ROW,
	COMMAND_LAST_ROW,
	COMMAND_BEYOND,
	COMMAND_REFRESH,
	COMMAND_REFUSED,
	COMMAND_GO_TO_MARK,
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

/* Kinds */

/*
 * The application keeps the data its rows show: a list has no call that
 * returns a row's text. The table travels with the screen as its data.
 */
struct row_table {
	const char *const *texts;
	size_t count;
};

static struct row_table fruit_table = { fruits, APIDEMO_COUNT(fruits) };

/*
 * OK on a row of an IMPLICIT list arrives as FPLINUX_UI_SELECT_COMMAND;
 * the focused row is the one chosen.
 */
static void implicit_command(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *list, int command_id,
			     void *context)
{
	const struct row_table *table = fplinux_ui_screen_get_data(list);
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND && row < table->count)
		apidemo_showf(context, _("Selected"),
			      _("Index %zu: %s.\nCommand: "
				"FPLINUX_UI_SELECT_COMMAND"),
			      row, _(table->texts[row]));
}

static void open_implicit(struct apidemo *app,
			  const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_IMPLICIT);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	for (size_t i = 0; i < fruit_table.count; ++i)
		fplinux_ui_list_append(list, _(fruit_table.texts[i]), NULL);
	fplinux_ui_screen_set_data(list, &fruit_table);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, implicit_command, NULL, app);
	apidemo_push(app, list, "fplinux_ui_list_new");
}

/* Each row sets the centre legend that it names. */
struct select_label {
	const char *text;
	/* The C argument, shown as the row's detail. */
	const char *detail;
	/* An N_() label; NULL restores "Select". */
	const char *label;
	/* A label with a control character, which the runtime refuses. */
	bool invalid;
};

static const struct select_label select_labels[] = {
	{ N_("Default label"), "NULL", NULL, false },
	{ N_("Open"), "_(\"Open\")", N_("Open"), false },
	{ N_("Play"), "_(\"Play\")", N_("Play"), false },
	{ N_("Call"), "_(\"Call\")", N_("Call"), false },
	{ N_("Two lines"), "\"Two\\nlines\"", "Two\nlines", true },
};

/* The legend changes at once; a refused label keeps the one before. */
static void select_label_command(struct fplinux_ui *ui,
				 struct fplinux_ui_screen *list, int command_id,
				 void *context)
{
	size_t row = fplinux_ui_list_get_focused(list);
	const struct select_label *choice;
	const char *label;

	if (command_id == APIDEMO_COMMAND_BACK) {
		fplinux_ui_go_back(ui);
		return;
	}
	if (command_id != FPLINUX_UI_SELECT_COMMAND ||
	    row >= APIDEMO_COUNT(select_labels))
		return;
	choice = &select_labels[row];
	label = choice->label && !choice->invalid ? _(choice->label) :
						    choice->label;
	if (!fplinux_ui_list_set_select_label(list, label))
		apidemo_showf(context, _("Label refused"),
			      "fplinux_ui_list_set_select_label() = false\n"
			      "errno = %s",
			      apidemo_errno_name(errno));
}

static void open_select_label(struct apidemo *app,
			      const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_IMPLICIT);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(select_labels); ++i)
		fplinux_ui_list_append(list, _(select_labels[i].text),
				       select_labels[i].detail);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, select_label_command, NULL, app);
	apidemo_push(app, list, "fplinux_ui_list_new");
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
		    _("Unmark"), 1, NULL);
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
		    _("Count marks"), 1, NULL);
	add_command(list, COMMAND_UNMARK_ALL, FPLINUX_UI_COMMAND_SCREEN,
		    _("Unmark all"), 2, NULL);
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
				list, i, apidemo_glyph(app, settings[i].glyph));
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
		    _("Rename"), 1, NULL);
	add_command(list, COMMAND_NO_DETAIL, FPLINUX_UI_COMMAND_SCREEN,
		    _("Remove detail"), 2, NULL);
	add_command(list, COMMAND_ADD_ICONS, FPLINUX_UI_COMMAND_SCREEN,
		    _("Add icons"), 3, NULL);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, details_command, NULL, app);
	apidemo_push(app, list, "fplinux_ui_list_new");
}

/* Rows: icons */

/* Each row's icon and where it comes from; the detail names the source. */
struct icon_row {
	const char *text;
	const char *detail;
	/* A system glyph name, else a PNG path, else no icon. */
	const char *glyph;
	const char *path;
};

static const struct icon_row icon_rows[] = {
	{ N_("Save"), "fplinux_ui_glyph(\"save\")", "save", NULL },
	{ N_("Search"), "fplinux_ui_glyph(\"search\")", "search", NULL },
	/* Alpha-only: drawn in the row's text colour, inverted on focus. */
	{ N_("Star"), "star-symbolic.png", NULL, APIDEMO_SHAPE },
	/* A colour picture, scaled down to the icon box. */
	{ N_("Photo"), "photo.png", NULL, APIDEMO_PHOTO },
	/* Its text still lines up: the empty box stays. */
	{ N_("No icon"), "NULL", NULL, NULL },
	{ N_("Missing file"), "missing.png", NULL,
	  APIDEMO_IMAGE_DIR "missing.png" },
	/* A file that exists but is no image: the app's desktop entry. */
	{ N_("Not a picture"), "apidemo.desktop", NULL,
	  "/usr/share/applications/" APIDEMO_APP_ID ".desktop" },
};

static const void *icon_source(struct apidemo *app, const struct icon_row *row)
{
	return row->glyph ? apidemo_glyph(app, row->glyph) : row->path;
}

/* Sets the row's icon; a failure keeps the icon and names its errno. */
static bool set_row_icon(struct apidemo *app, struct fplinux_ui_screen *list,
			 size_t row)
{
	const struct icon_row *icon = &icon_rows[row];
	char detail[64];

	if (fplinux_ui_list_set_icon(list, row, icon_source(app, icon)))
		return true;
	snprintf(detail, sizeof(detail), "%s: %s", icon->detail,
		 apidemo_errno_name(errno));
	fplinux_ui_list_set(list, row, _(icon->text), detail);
	return false;
}

static void icons_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			  int command_id, void *context)
{
	struct apidemo *app = context;
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK) {
		fplinux_ui_go_back(ui);
		return;
	}
	if (row >= APIDEMO_COUNT(icon_rows))
		return;
	if (command_id == FPLINUX_UI_SELECT_COMMAND &&
	    !set_row_icon(app, list, row))
		apidemo_showf(app, _("No icon"),
			      "fplinux_ui_list_set_icon() = false\n"
			      "errno = %s",
			      apidemo_errno_name(errno));
	else if (command_id == COMMAND_REMOVE_ICON)
		fplinux_ui_list_set_icon(list, row, NULL);
}

static void open_icons(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_IMPLICIT);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(icon_rows); ++i) {
		fplinux_ui_list_append(list, _(icon_rows[i].text),
				       icon_rows[i].detail);
		set_row_icon(app, list, i);
	}
	/* OK sets the focused row's icon again, the left key removes it. */
	fplinux_ui_list_set_select_label(list, _("Set"));
	add_command(list, COMMAND_REMOVE_ICON, FPLINUX_UI_COMMAND_SCREEN,
		    _("Remove"), 1, NULL);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, icons_command, NULL, app);
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
	/* The focus goes to the row that takes its index, or the last row. */
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

/* Indices beyond the rows fail and change nothing. */
static void show_out_of_range(struct apidemo *app,
			      struct fplinux_ui_screen *list)
{
	size_t size = fplinux_ui_list_size(list);
	char row[24];
	size_t inserted;
	bool deleted;
	bool set;
	int insert_error;
	int delete_error;
	int set_error;

	errno = 0;
	inserted = fplinux_ui_list_insert(list, size + 1, "-", NULL);
	insert_error = errno;
	errno = 0;
	deleted = fplinux_ui_list_delete(list, size);
	delete_error = errno;
	errno = 0;
	set = fplinux_ui_list_set(list, size, "-", NULL);
	set_error = errno;
	apidemo_showf(app, _("Out of range"),
		      "insert(%zu) = %s, %s\n"
		      "delete(%zu) = %s, %s\n"
		      "set(%zu) = %s, %s",
		      size + 1, index_name(inserted, row, sizeof(row)),
		      apidemo_errno_name(insert_error), size,
		      deleted ? "true" : "false",
		      apidemo_errno_name(delete_error), size,
		      set ? "true" : "false", apidemo_errno_name(set_error));
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
	case COMMAND_OUT_OF_RANGE:
		show_out_of_range(app, list);
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
		    _("Size and focus"), 7, NULL);
	add_command(list, COMMAND_OUT_OF_RANGE, FPLINUX_UI_COMMAND_SCREEN,
		    _("Out of range"), 8, NULL);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, rows_command, NULL, app);
	apidemo_push(app, list, "fplinux_ui_list_new");
}

/* Rows: paging */

#define PAGING_ROWS 60

static void paging_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, int command_id,
			   void *context)
{
	size_t size = fplinux_ui_list_size(list);

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		return;
	case COMMAND_FIRST_ROW:
		fplinux_ui_list_set_focused(list, 0);
		return;
	case COMMAND_MIDDLE_ROW:
		fplinux_ui_list_set_focused(list, size / 2);
		return;
	case COMMAND_LAST_ROW:
		fplinux_ui_list_set_focused(list, size - 1);
		return;
	case COMMAND_BEYOND:
		/* An index out of range leaves the focus where it was. */
		fplinux_ui_list_set_focused(list, size);
		break;
	case FPLINUX_UI_SELECT_COMMAND:
		break;
	default:
		return;
	}
	apidemo_showf(context, _("Focus"),
		      "fplinux_ui_list_get_focused() = %zu",
		      fplinux_ui_list_get_focused(list));
}

static void open_paging(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_IMPLICIT);
	char text[32];

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	/* Up and Down move by a row, Left and Right by a visible page. */
	for (int i = 1; i <= PAGING_ROWS; ++i) {
		snprintf(text, sizeof(text), _("Row %d"), i);
		fplinux_ui_list_append(list, text, NULL);
	}
	add_command(list, COMMAND_FIRST_ROW, FPLINUX_UI_COMMAND_SCREEN,
		    _("First row"), 1, NULL);
	add_command(list, COMMAND_MIDDLE_ROW, FPLINUX_UI_COMMAND_SCREEN,
		    _("Middle row"), 2, NULL);
	add_command(list, COMMAND_LAST_ROW, FPLINUX_UI_COMMAND_SCREEN,
		    _("Last row"), 3, NULL);
	add_command(list, COMMAND_BEYOND, FPLINUX_UI_COMMAND_SCREEN,
		    _("Past the end"), 4, NULL);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, paging_command, NULL, app);
	apidemo_push(app, list, "fplinux_ui_list_new");
}

/* Virtual lists */

/*
 * A virtual list stores no rows: the runtime asks the row callback for
 * each row that comes into view. The callback writes a field only when
 * FPLINUX_UI_VIRTUAL_ROW_HAS() says that the runtime has it. The runtime
 * copies the strings after the callback returns, so they live in static
 * buffers, never in local arrays of the callback.
 */
#define VIRTUAL_ROWS 100000
#define SECONDS_PER_MINUTE 60

static char virtual_text[64];
static char virtual_detail[64];

/* The event log: event n + 1 happened n minutes before log_time. */
static time_t log_time;

static time_t event_time(size_t index)
{
	return log_time - (time_t)index * SECONDS_PER_MINUTE;
}

static bool event_row(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
		      size_t index, struct fplinux_ui_virtual_row *row,
		      void *context)
{
	struct apidemo *app = context;

	(void)list;
	if (index >= VIRTUAL_ROWS)
		return false;
	snprintf(virtual_text, sizeof(virtual_text), _("Event %zu"), index + 1);
	row->text = virtual_text;
	/* Every row has a detail, so the pitch never changes. */
	if (FPLINUX_UI_VIRTUAL_ROW_HAS(row, detail) &&
	    fplinux_ui_format_date(ui, event_time(index), FPLINUX_UI_DATE_CLOCK,
				   virtual_detail, sizeof(virtual_detail)))
		row->detail = virtual_detail;
	/* A system glyph costs least: icons are decoded as rows show. */
	if (FPLINUX_UI_VIRTUAL_ROW_HAS(row, icon))
		row->icon = apidemo_glyph(app, "about");
	return true;
}

/* The stored-list calls that a virtual list refuses with EINVAL. */
static void show_refused(struct apidemo *app, struct fplinux_ui_screen *list)
{
	int errors[6];
	int count = 0;

	errno = 0;
	fplinux_ui_list_append(list, "-", NULL);
	errors[count++] = errno;
	errno = 0;
	fplinux_ui_list_insert(list, 0, "-", NULL);
	errors[count++] = errno;
	errno = 0;
	fplinux_ui_list_set(list, 0, "-", NULL);
	errors[count++] = errno;
	errno = 0;
	fplinux_ui_list_delete(list, 0);
	errors[count++] = errno;
	errno = 0;
	fplinux_ui_list_set_icon(list, 0, apidemo_glyph(app, "add"));
	errors[count++] = errno;
	errno = 0;
	fplinux_ui_list_set_selected(list, 0, true);
	errors[count++] = errno;
	/* delete_all() does nothing: the count stays. */
	fplinux_ui_list_delete_all(list);
	apidemo_showf(
		app, _("Refused calls"),
		"append: %s\ninsert: %s\nset: %s\ndelete: %s\n"
		"set_icon: %s\nset_selected: %s\n"
		"delete_all: size %zu",
		apidemo_errno_name(errors[0]), apidemo_errno_name(errors[1]),
		apidemo_errno_name(errors[2]), apidemo_errno_name(errors[3]),
		apidemo_errno_name(errors[4]), apidemo_errno_name(errors[5]),
		fplinux_ui_list_size(list));
}

static void events_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, int command_id,
			   void *context)
{
	struct apidemo *app = context;
	size_t row = fplinux_ui_list_get_focused(list);
	char when[64];

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case FPLINUX_UI_SELECT_COMMAND:
		if (!fplinux_ui_format_date(ui, event_time(row),
					    FPLINUX_UI_DATE_LONG, when,
					    sizeof(when)))
			when[0] = '\0';
		apidemo_showf(app, _("Selected"), _("Event %zu: %s."), row + 1,
			      when);
		break;
	case COMMAND_REFRESH:
		/* The data changed: the visible rows are asked again. */
		log_time = time(NULL);
		fplinux_ui_list_invalidate(list, 0, fplinux_ui_list_size(list));
		break;
	case COMMAND_LAST_ROW:
		/* Focus works by index, as on a stored list. */
		fplinux_ui_list_set_focused(list,
					    fplinux_ui_list_size(list) - 1);
		break;
	case COMMAND_REFUSED:
		show_refused(app, list);
		break;
	default:
		break;
	}
}

static void open_virtual(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list;

	log_time = time(NULL);
	list = fplinux_ui_list_new_virtual(app->ui, _(entry->title),
					   FPLINUX_UI_LIST_IMPLICIT,
					   VIRTUAL_ROWS, event_row, app);
	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new_virtual");
		return;
	}
	add_command(list, COMMAND_REFRESH, FPLINUX_UI_COMMAND_SCREEN,
		    _("Refresh"), 1, NULL);
	add_command(list, COMMAND_LAST_ROW, FPLINUX_UI_COMMAND_SCREEN,
		    _("Last row"), 2, NULL);
	add_command(list, COMMAND_REFUSED, FPLINUX_UI_COMMAND_SCREEN,
		    _("Refused calls"), 3, NULL);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, events_command, NULL, app);
	apidemo_push(app, list, "fplinux_ui_list_new_virtual");
}

/*
 * A virtual EXCLUSIVE list: the runtime keeps no marks. OK calls the
 * change listener, and the application moves its one mark and invalidates
 * the two rows whose mark changed.
 */
static size_t virtual_choice;

static bool option_row(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
		       size_t index, struct fplinux_ui_virtual_row *row,
		       void *context)
{
	(void)ui;
	(void)list;
	(void)context;
	if (index >= VIRTUAL_ROWS)
		return false;
	snprintf(virtual_text, sizeof(virtual_text), _("Option %zu"),
		 index + 1);
	row->text = virtual_text;
	if (FPLINUX_UI_VIRTUAL_ROW_HAS(row, marked))
		row->marked = index == virtual_choice;
	return true;
}

static void option_changed(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, size_t index,
			   void *context)
{
	size_t before = virtual_choice;

	(void)ui;
	(void)context;
	virtual_choice = index;
	fplinux_ui_list_invalidate(list, before, 1);
	fplinux_ui_list_invalidate(list, index, 1);
}

static void option_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, int command_id,
			   void *context)
{
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_GO_TO_MARK)
		fplinux_ui_list_set_focused(list, virtual_choice);
}

static void open_virtual_exclusive(struct apidemo *app,
				   const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list;

	virtual_choice = VIRTUAL_ROWS / 2;
	list = fplinux_ui_list_new_virtual(app->ui, _(entry->title),
					   FPLINUX_UI_LIST_EXCLUSIVE,
					   VIRTUAL_ROWS, option_row, app);
	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new_virtual");
		return;
	}
	fplinux_ui_list_set_focused(list, virtual_choice);
	add_command(list, COMMAND_GO_TO_MARK, FPLINUX_UI_COMMAND_SCREEN,
		    _("Go to mark"), 1, NULL);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, option_command, option_changed,
				       app);
	apidemo_push(app, list, "fplinux_ui_list_new_virtual");
}

/* A virtual MULTIPLE list: one bit per row in the application's data. */
static unsigned char virtual_marks[(VIRTUAL_ROWS + 7) / 8];

static bool is_marked(size_t index)
{
	return virtual_marks[index / 8] & (1U << (index % 8));
}

static bool item_row(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
		     size_t index, struct fplinux_ui_virtual_row *row,
		     void *context)
{
	(void)ui;
	(void)list;
	(void)context;
	if (index >= VIRTUAL_ROWS)
		return false;
	snprintf(virtual_text, sizeof(virtual_text), _("Item %zu"), index + 1);
	row->text = virtual_text;
	if (FPLINUX_UI_VIRTUAL_ROW_HAS(row, marked))
		row->marked = is_marked(index);
	return true;
}

static void item_changed(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			 size_t index, void *context)
{
	(void)ui;
	(void)context;
	virtual_marks[index / 8] ^= (unsigned char)(1U << (index % 8));
	fplinux_ui_list_invalidate(list, index, 1);
}

static void item_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			 int command_id, void *context)
{
	size_t marked = 0;

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_COUNT:
		for (size_t i = 0; i < VIRTUAL_ROWS; ++i)
			marked += is_marked(i);
		apidemo_showf(context, _("Marked rows"),
			      ngettext("%zu row is marked.",
				       "%zu rows are marked.", marked),
			      marked);
		break;
	case COMMAND_UNMARK_ALL:
		memset(virtual_marks, 0, sizeof(virtual_marks));
		fplinux_ui_list_invalidate(list, 0, fplinux_ui_list_size(list));
		break;
	default:
		break;
	}
}

static void open_virtual_multiple(struct apidemo *app,
				  const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list;

	memset(virtual_marks, 0, sizeof(virtual_marks));
	list = fplinux_ui_list_new_virtual(app->ui, _(entry->title),
					   FPLINUX_UI_LIST_MULTIPLE,
					   VIRTUAL_ROWS, item_row, app);
	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new_virtual");
		return;
	}
	add_command(list, COMMAND_COUNT, FPLINUX_UI_COMMAND_SCREEN,
		    _("Count marks"), 1, NULL);
	add_command(list, COMMAND_UNMARK_ALL, FPLINUX_UI_COMMAND_SCREEN,
		    _("Unmark all"), 2, NULL);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, item_command, item_changed, app);
	apidemo_push(app, list, "fplinux_ui_list_new_virtual");
}

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
	case COMMAND_DETAILS: {
		size_t row = fplinux_ui_screen_get_command_item(list);
		char message[64];

		if (row >= inbox_count)
			break;
		snprintf(message, sizeof(message), _("Message %zu"),
			 inbox_newest - row);
		apidemo_showf(context, _("Details"),
			      "fplinux_ui_screen_get_command_item() = %zu\n%s",
			      row, message);
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

/*
 * Without an item listener, item commands reach the command listener too;
 * fplinux_ui_screen_get_command_item() names their row, and returns
 * SIZE_MAX for a screen command such as Back or Select.
 */
static void contact_any_command(struct fplinux_ui *ui,
				struct fplinux_ui_screen *list, int command_id,
				void *context)
{
	size_t row = fplinux_ui_screen_get_command_item(list);
	size_t focus = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_CALL)
		apidemo_showf(context, _("Call"),
			      _("Calling %s.\n"
				"fplinux_ui_screen_get_command_item() = %zu"),
			      contact_name(row), row);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND && focus != SIZE_MAX)
		apidemo_showf(context, _("Selected"),
			      _("%s has no number to call.\n"
				"fplinux_ui_screen_get_command_item() = "
				"SIZE_MAX"),
			      contact_name(focus));
	else if (row != SIZE_MAX)
		contact_action(list, row, command_id);
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
	/* variant 0 has an item listener; variant 1 has none. */
	if (entry->variant == 0) {
		fplinux_ui_screen_set_listener(list, contact_command, NULL,
					       app);
		fplinux_ui_screen_set_item_listener(list, contact_item, app);
	} else {
		fplinux_ui_screen_set_listener(list, contact_any_command, NULL,
					       app);
	}
	apidemo_push(app, list, "fplinux_ui_list_new");
}

static const struct apidemo_entry entries[] = {
	{
		.title = N_("Implicit list"),
		.detail = "fplinux_ui_list_new · FPLINUX_UI_LIST_IMPLICIT · "
			  "FPLINUX_UI_SELECT_COMMAND · "
			  "fplinux_ui_screen_set_data",
		.open = open_implicit,
	},
	{
		.title = N_("Select label"),
		.detail = "fplinux_ui_list_set_select_label",
		.open = open_select_label,
	},
	{
		.title = N_("Exclusive list"),
		.detail = "FPLINUX_UI_LIST_EXCLUSIVE · "
			  "fplinux_ui_list_set_selected · "
			  "fplinux_ui_list_is_selected",
		.open = open_exclusive,
	},
	{
		.title = N_("Multiple list"),
		.detail = "FPLINUX_UI_LIST_MULTIPLE · "
			  "fplinux_ui_screen_set_listener · "
			  "fplinux_ui_list_is_selected",
		.open = open_multiple,
	},
	{
		.title = N_("Row details"),
		.detail = "fplinux_ui_list_append · fplinux_ui_list_set",
		.open = open_details,
	},
	{
		.title = N_("Row icons"),
		.detail = "fplinux_ui_list_set_icon · fplinux_ui_glyph",
		.open = open_icons,
	},
	{
		.title = N_("Changing rows"),
		.detail = "fplinux_ui_list_insert · fplinux_ui_list_delete · "
			  "fplinux_ui_list_delete_all · fplinux_ui_list_size",
		.open = open_rows,
	},
	{
		.title = N_("Paging"),
		.detail = "fplinux_ui_list_set_focused · "
			  "fplinux_ui_list_get_focused",
		.open = open_paging,
	},
	{
		.title = N_("Virtual list"),
		.detail = "fplinux_ui_list_new_virtual · "
			  "FPLINUX_UI_VIRTUAL_ROW_HAS · "
			  "fplinux_ui_list_invalidate",
		.open = open_virtual,
	},
	{
		.title = N_("Virtual exclusive"),
		.detail = "fplinux_ui_list_new_virtual · "
			  "FPLINUX_UI_LIST_EXCLUSIVE · "
			  "fplinux_ui_list_invalidate",
		.open = open_virtual_exclusive,
	},
	{
		.title = N_("Virtual multiple"),
		.detail = "fplinux_ui_list_new_virtual · "
			  "FPLINUX_UI_LIST_MULTIPLE · "
			  "fplinux_ui_list_invalidate",
		.open = open_virtual_multiple,
	},
	{
		.title = N_("Virtual changes"),
		.detail = "fplinux_ui_list_rows_inserted · "
			  "fplinux_ui_list_rows_deleted",
		.open = open_virtual_changes,
	},
	{
		.title = N_("Item commands"),
		.detail = "fplinux_ui_list_add_item_command · "
			  "fplinux_ui_screen_set_item_listener · "
			  "fplinux_ui_screen_remove_item_command",
		.open = open_item_commands,
		.variant = 0,
	},
	{
		.title = N_("Without item listener"),
		.detail = "fplinux_ui_list_add_item_command · "
			  "fplinux_ui_screen_get_command_item",
		.open = open_item_commands,
		.variant = 1,
	},
};

const struct apidemo_category apidemo_lists = {
	.title = N_("Lists"),
	.icon = APIDEMO_IMAGE_DIR "lists-symbolic.png",
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
};
