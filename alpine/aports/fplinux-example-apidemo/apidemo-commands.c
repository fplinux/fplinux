/* SPDX-License-Identifier: 0BSD */
/*
 * Commands and menus: command types, soft-key legends, the options menu.
 *
 * A screen never sets its legends itself. It carries typed commands, and
 * the runtime assigns them to the keys every time the screen, its commands
 * or its focus change:
 *
 *   right soft key  the first BACK, CANCEL, EXIT or STOP command by priority;
 *   OK              the focused element's own action, else the first OK;
 *   left soft key   the one remaining command, else Options, which opens the
 *                   options menu with all the remaining ones by priority.
 *
 * Most demos here are forms of read-only notes, so that OK has no element
 * action of its own and the notes can explain what the keys show.
 */
#include "apidemo.h"

#include <errno.h>
#include <stdio.h>

/* A note of a demo form: a label, which may be NULL, and its text. */
struct note {
	const char *label;
	const char *text;
};

/* A form with the entry's title and the notes, translated on use. */
static struct fplinux_ui_screen *notes_form(struct apidemo *app,
					    const struct apidemo_entry *entry,
					    const struct note *notes,
					    size_t count)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return NULL;
	}
	for (size_t i = 0; i < count; i++)
		fplinux_ui_form_append_string(
			form, notes[i].label ? _(notes[i].label) : NULL,
			_(notes[i].text));
	return form;
}

/* Every type */

static const char *const type_names[] = {
	[FPLINUX_UI_COMMAND_BACK] = "BACK",
	[FPLINUX_UI_COMMAND_CANCEL] = "CANCEL",
	[FPLINUX_UI_COMMAND_EXIT] = "EXIT",
	[FPLINUX_UI_COMMAND_STOP] = "STOP",
	[FPLINUX_UI_COMMAND_OK] = "OK",
	[FPLINUX_UI_COMMAND_ITEM] = "ITEM",
	[FPLINUX_UI_COMMAND_SCREEN] = "SCREEN",
	[FPLINUX_UI_COMMAND_HELP] = "HELP",
};

/* The commands' ids are their types; Back is the shared one. */
static void type_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context)
{
	(void)screen;
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id >= 0 &&
		 (size_t)command_id < APIDEMO_COUNT(type_names))
		apidemo_showf(context, _("Command"),
			      _("The listener received id %d, type %s."),
			      command_id, type_names[command_id]);
}

/* ITEM and SCREEN need a label; NULL gives the others the runtime's word. */
static const char *type_label(int type)
{
	if (type == FPLINUX_UI_COMMAND_ITEM)
		return _("Item");
	return type == FPLINUX_UI_COMMAND_SCREEN ? _("Screen") : NULL;
}

static void open_types(struct apidemo *app, const struct apidemo_entry *entry)
{
	static const struct note notes[] = {
		{ N_("Right soft key"),
		  N_("The first BACK, CANCEL, EXIT or STOP command by "
		     "priority: here Back.") },
		{ N_("OK"),
		  N_("The focused element's own action, otherwise the first "
		     "OK command.") },
		{ N_("Left soft key"),
		  N_("The one remaining command, otherwise Options with all "
		     "the others: here Cancel, Exit, Stop, Item, Screen and "
		     "Help.") },
		{ N_("Labels"),
		  N_("A NULL label shows the type's word in the system "
		     "language. ITEM and SCREEN commands need their own.") },
	};
	struct fplinux_ui_screen *form =
		notes_form(app, entry, notes, APIDEMO_COUNT(notes));

	if (!form)
		return;
	apidemo_add_back(form);
	/* Equal priorities: the menu keeps the order of addition. */
	for (int type = FPLINUX_UI_COMMAND_CANCEL;
	     type <= FPLINUX_UI_COMMAND_HELP; type++) {
		const struct fplinux_ui_command command = {
			.id = type,
			.type = (enum fplinux_ui_command_type)type,
			.label = type_label(type),
		};

		fplinux_ui_screen_add_command(form, &command);
	}
	fplinux_ui_screen_set_listener(form, type_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Options menu with icons */

/* SCREEN commands with a system glyph; one has none. */
static const struct {
	const char *label;
	const char *glyph;
} icon_commands[] = {
	{ N_("Save"), "save" }, { N_("Send"), "send" },
	{ N_("Edit"), "edit" }, { N_("Search"), "search" },
	{ N_("Add"), "add" },	{ N_("Delete"), "delete" },
	{ N_("Rename"), NULL },
};

#define COMMAND_HELP 100

static void navigate_help(struct apidemo *app, const char *text)
{
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_INFO, _("Help"), text, 0);

	if (!alert) {
		apidemo_error(app, "fplinux_ui_alert_new");
		return;
	}
	fplinux_ui_navigate(app->ui, alert);
}

static void icon_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context)
{
	struct apidemo *app = context;

	(void)screen;
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_HELP)
		navigate_help(app,
			      _("Each command shows its glyph in the options "
				"menu. Rename has none: its text still lines "
				"up with the others."));
	else if (command_id >= 1 &&
		 (size_t)command_id <= APIDEMO_COUNT(icon_commands))
		apidemo_showf(app, _(icon_commands[command_id - 1].label),
			      _("The listener received id %d."), command_id);
}

static void open_icons(struct apidemo *app, const struct apidemo_entry *entry)
{
	static const struct note notes[] = {
		{ NULL,
		  N_("Options opens the menu. Each command there shows its "
		     "icon, a system glyph from fplinux_ui_glyph(), in the "
		     "row's text colour.") },
	};
	const struct fplinux_ui_command help = {
		.id = COMMAND_HELP,
		.type = FPLINUX_UI_COMMAND_HELP,
		.priority = (int)APIDEMO_COUNT(icon_commands),
		.icon = apidemo_glyph(app, "help"),
	};
	struct fplinux_ui_screen *form =
		notes_form(app, entry, notes, APIDEMO_COUNT(notes));

	if (!form)
		return;
	apidemo_add_back(form);
	for (size_t i = 0; i < APIDEMO_COUNT(icon_commands); i++) {
		const struct fplinux_ui_command command = {
			.id = (int)i + 1,
			.type = FPLINUX_UI_COMMAND_SCREEN,
			.label = _(icon_commands[i].label),
			.priority = (int)i,
			/* The runtime decodes the icon now, at row size. */
			.icon = icon_commands[i].glyph ?
					apidemo_glyph(app,
						      icon_commands[i].glyph) :
					NULL,
		};

		fplinux_ui_screen_add_command(form, &command);
	}
	fplinux_ui_screen_add_command(form, &help);
	fplinux_ui_screen_set_listener(form, icon_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Priorities and ties */

static const char *const ranked_labels[] = {
	N_("Priority 2"),
	N_("Priority 1, added first"),
	N_("Priority 0"),
	N_("Priority 1, added second"),
};
static const int ranked_priorities[] = { 2, 1, 0, 1 };

#define COMMAND_EXIT 100

static void ranked_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int command_id,
			   void *context)
{
	(void)screen;
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_EXIT)
		apidemo_show(context, _("Exit"),
			     _("An application would leave here, with "
			       "fplinux_ui_back()."));
	else if (command_id >= 1 &&
		 (size_t)command_id <= APIDEMO_COUNT(ranked_labels))
		apidemo_show(context, _("Chosen"),
			     _(ranked_labels[command_id - 1]));
}

static void open_ranked(struct apidemo *app, const struct apidemo_entry *entry)
{
	static const struct note notes[] = {
		{ NULL,
		  N_("The commands were added with the priorities 2, 1, 0 "
		     "and 1. The options menu lists the lower numbers first; "
		     "the two of priority 1 keep the order of addition.") },
		{ NULL, N_("Back has priority 0 and takes the right soft key. "
			   "Exit, priority 3, goes to the menu, last.") },
	};
	const struct fplinux_ui_command exit_command = {
		.id = COMMAND_EXIT,
		.type = FPLINUX_UI_COMMAND_EXIT,
		.priority = 3,
	};
	struct fplinux_ui_screen *form =
		notes_form(app, entry, notes, APIDEMO_COUNT(notes));

	if (!form)
		return;
	fplinux_ui_screen_add_command(form, &exit_command);
	for (size_t i = 0; i < APIDEMO_COUNT(ranked_labels); i++) {
		const struct fplinux_ui_command command = {
			.id = (int)i + 1,
			.type = FPLINUX_UI_COMMAND_SCREEN,
			.label = _(ranked_labels[i]),
			.priority = ranked_priorities[i],
		};

		fplinux_ui_screen_add_command(form, &command);
	}
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, ranked_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* One left command: on the key when its legend fits, else in the menu */

#define COMMAND_SAVE 1

static void save_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context)
{
	(void)screen;
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_SAVE)
		apidemo_show(context, _("Saved"),
			     _("The command reached the listener as usual."));
}

static void open_left(struct apidemo *app, const struct apidemo_entry *entry)
{
	static const struct note fits[] = {
		{ NULL,
		  N_("A single remaining command goes on the left soft key "
		     "itself when its legend fits beside the others.") },
	};
	static const struct note collapses[] = {
		{ NULL,
		  N_("This label is too long for the soft-key bar, so the "
		     "left soft key shows Options and the command becomes "
		     "the first item of the menu. Prefer a short word.") },
	};
	const struct fplinux_ui_command save = {
		.id = COMMAND_SAVE,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label =
			entry->variant ?
				_("Save the draft and send it to all contacts") :
				_("Save"),
	};
	struct fplinux_ui_screen *form =
		entry->variant ?
			notes_form(app, entry, collapses,
				   APIDEMO_COUNT(collapses)) :
			notes_form(app, entry, fits, APIDEMO_COUNT(fits));

	if (!form)
		return;
	apidemo_add_back(form);
	fplinux_ui_screen_add_command(form, &save);
	fplinux_ui_screen_set_listener(form, save_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Help */

static void help_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context)
{
	(void)screen;
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_HELP)
		navigate_help(context,
			      _("A HELP command opens the application's own "
				"help, such as this alert."));
}

static void open_help(struct apidemo *app, const struct apidemo_entry *entry)
{
	static const struct note notes[] = {
		{ NULL,
		  N_("A HELP command always goes to the options menu, so the "
		     "left soft key shows Options even though Help is the "
		     "only command there.") },
	};
	const struct fplinux_ui_command help = {
		.id = COMMAND_HELP,
		.type = FPLINUX_UI_COMMAND_HELP,
	};
	struct fplinux_ui_screen *form =
		notes_form(app, entry, notes, APIDEMO_COUNT(notes));

	if (!form)
		return;
	apidemo_add_back(form);
	fplinux_ui_screen_add_command(form, &help);
	fplinux_ui_screen_set_listener(form, help_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Relabelling */

#define COMMAND_RUN 1

/* The form's state item and whether the "activity" runs. */
static struct {
	size_t state;
	bool running;
} relabel;

/* Adding a command with an id the screen has replaces it: a new label. */
static void set_run_label(struct fplinux_ui_screen *form, bool running)
{
	const struct fplinux_ui_command run = {
		.id = COMMAND_RUN,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = running ? _("Pause") : _("Start"),
	};

	fplinux_ui_screen_add_command(form, &run);
	fplinux_ui_form_set_string(form, relabel.state,
				   running ? _("Running") : _("Paused"));
}

static void run_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			int command_id, void *context)
{
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK) {
		fplinux_ui_go_back(ui);
	} else if (command_id == COMMAND_RUN) {
		relabel.running = !relabel.running;
		set_run_label(form, relabel.running);
	}
}

static void open_relabel(struct apidemo *app, const struct apidemo_entry *entry)
{
	static const struct note notes[] = {
		{ NULL,
		  N_("OK switches between Start and Pause: the listener adds "
		     "the command again with the same id and a new label.") },
	};
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	relabel.running = false;
	/* The state first, where the smallest screen shows it. */
	relabel.state = fplinux_ui_form_append_string(form, _("State"), "");
	fplinux_ui_form_append_string(form, NULL, _(notes[0].text));
	apidemo_add_back(form);
	set_run_label(form, false);
	fplinux_ui_screen_set_listener(form, run_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Adding and removing commands */

#define COMMAND_ADD 100
#define ADDED_LIMIT 5

/* Which of the commands 1 .. ADDED_LIMIT the screen has. */
static struct {
	bool present[ADDED_LIMIT];
	int count;
} added;

static void add_add_command(struct fplinux_ui_screen *form)
{
	const struct fplinux_ui_command add = {
		.id = COMMAND_ADD,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Add"),
	};

	fplinux_ui_screen_add_command(form, &add);
}

/* The title counts the commands; the runtime copies it. */
static void count_commands(struct fplinux_ui_screen *form)
{
	char title[64];

	snprintf(title, sizeof(title),
		 ngettext("%d command", "%d commands",
			  (unsigned long)added.count),
		 added.count);
	fplinux_ui_screen_set_title(form, title);
}

static void added_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK) {
		fplinux_ui_go_back(ui);
		return;
	}
	if (command_id == COMMAND_ADD) {
		for (int i = 0; i < ADDED_LIMIT; i++) {
			char label[64];
			struct fplinux_ui_command command = {
				.id = i + 1,
				.type = FPLINUX_UI_COMMAND_SCREEN,
				.label = label,
				/* By number, whenever it was added. */
				.priority = i,
			};

			if (added.present[i])
				continue;
			/* Short, so that one fits on the left soft key. */
			snprintf(label, sizeof(label), _("No. %d"), i + 1);
			if (!fplinux_ui_screen_add_command(form, &command))
				break;
			added.present[i] = true;
			added.count++;
			break;
		}
		/* The last one taken: OK has nothing more to add. */
		if (added.count == ADDED_LIMIT)
			fplinux_ui_screen_remove_command(form, COMMAND_ADD);
	} else if (command_id >= 1 && command_id <= ADDED_LIMIT) {
		fplinux_ui_screen_remove_command(form, command_id);
		added.present[command_id - 1] = false;
		added.count--;
		add_add_command(form);
	}
	count_commands(form);
}

static void open_added(struct apidemo *app, const struct apidemo_entry *entry)
{
	static const struct note notes[] = {
		{ NULL,
		  N_("OK adds a command and choosing one removes it. One "
		     "command sits on the left soft key; two or more go to "
		     "Options. The title counts them.") },
	};
	struct fplinux_ui_screen *form =
		notes_form(app, entry, notes, APIDEMO_COUNT(notes));

	if (!form)
		return;
	for (int i = 0; i < ADDED_LIMIT; i++)
		added.present[i] = false;
	added.count = 0;
	apidemo_add_back(form);
	add_add_command(form);
	fplinux_ui_screen_set_listener(form, added_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Commands that fplinux_ui_screen_add_command() refuses */

#define COMMAND_VALID 1

static void valid_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int command_id,
			  void *context)
{
	(void)screen;
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_VALID)
		apidemo_show(context, _("OK"),
			     _("Only the valid command reached the screen."));
}

static void open_errors(struct apidemo *app, const struct apidemo_entry *entry)
{
	static const struct {
		const char *label;
		struct fplinux_ui_command command;
	} cases[] = {
		{ N_("ITEM without a label"),
		  { .id = 2, .type = FPLINUX_UI_COMMAND_ITEM } },
		{ N_("SCREEN without a label"),
		  { .id = 3, .type = FPLINUX_UI_COMMAND_SCREEN } },
		{ N_("The reserved select id"),
		  { .id = FPLINUX_UI_SELECT_COMMAND,
		    .type = FPLINUX_UI_COMMAND_OK } },
		{ N_("Unknown type"),
		  { .id = 4,
		    .type = (enum fplinux_ui_command_type)99,
		    .label = "99" } },
		{ N_("A line break in the label"),
		  { .id = 5,
		    .type = FPLINUX_UI_COMMAND_SCREEN,
		    .label = "Two\nlines" } },
		{ N_("OK with a NULL label"),
		  { .id = COMMAND_VALID, .type = FPLINUX_UI_COMMAND_OK } },
	};
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(cases); i++) {
		bool added_ok =
			fplinux_ui_screen_add_command(form, &cases[i].command);
		int error = errno;
		char result[64];

		if (added_ok)
			snprintf(result, sizeof(result), "true");
		else
			snprintf(result, sizeof(result), "false, %s",
				 apidemo_errno_name(error));
		fplinux_ui_form_append_string(form, _(cases[i].label), result);
	}
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, valid_command, NULL, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* The options menu on a canvas */

/*
 * On its own screen an application opens the system options menu with
 * fplinux_ui_menu_open(). A canvas could leave the left soft key without a
 * command and open the menu from its key handler, but the screen stack
 * names the keys after the commands and would show no legend for it. So
 * the left soft key is a SCREEN command labelled Options, whose listener
 * opens the menu.
 */
enum {
	COMMAND_OPTIONS = 1,
};

enum action {
	ACTION_SAVE = 1,
	ACTION_SEND,
	ACTION_EDIT,
	ACTION_SEARCH,
	ACTION_OTHER_MENU,
	ACTION_ADD,
	ACTION_DELETE,
	ACTION_HELP,
};

struct menu_entry {
	enum action action;
	const char *text;
	/* A system glyph, or NULL for an item without an icon. */
	const char *glyph;
};

static const struct menu_entry first_menu[] = {
	{ ACTION_SAVE, N_("Save"), "save" },
	{ ACTION_SEND, N_("Send"), "send" },
	{ ACTION_EDIT, N_("Edit"), "edit" },
	{ ACTION_SEARCH, N_("Search"), "search" },
	/* The callback replaces this popup with another menu. */
	{ ACTION_OTHER_MENU, N_("Other menu"), NULL },
};

static const struct menu_entry other_menu[] = {
	{ ACTION_ADD, N_("Add"), "add" },
	{ ACTION_DELETE, N_("Delete"), "delete" },
	{ ACTION_HELP, N_("Help"), "help" },
};

/* The timed variant closes the menu itself after MENU_MS. */
#define MENU_MS 3000

static struct {
	struct apidemo *app;
	/* The status label; NULL once the canvas is gone. */
	lv_obj_t *status;
	lv_obj_t *scroll;
	lv_timer_t *timer;
	bool timed;
	/* What happened, shown under the hint. */
	char result[160];
	char busy[160];
} menu_demo;

static void stop_menu_timer(void)
{
	if (menu_demo.timer)
		lv_timer_delete(menu_demo.timer);
	menu_demo.timer = NULL;
}

/* Draws only the label whose text changed. */
static void show_menu_status(void)
{
	char text[512];

	if (!menu_demo.status)
		return;
	/* The latest result first, where the smallest screen shows it. */
	snprintf(
		text, sizeof(text), "%s\n\n%s%s%s", menu_demo.result,
		menu_demo.busy, menu_demo.busy[0] ? "\n\n" : "",
		menu_demo.timed ?
			_("Options opens a menu that the application closes "
			  "after 3\u00a0s, unless you choose first.") :
			_("Options opens the menu. Other menu replaces it from "
			  "the callback; Back closes the current popup."));
	lv_label_set_text(menu_demo.status, text);
}

static void open_menu(const char *title, const struct menu_entry *entries,
		      size_t count);

/* The menu has closed before this call, so it may open another one. */
static void menu_chosen(struct fplinux_ui *ui, int action, void *context)
{
	(void)ui;
	(void)context;
	stop_menu_timer();
	if (action == ACTION_OTHER_MENU) {
		open_menu(_("Other menu"), other_menu,
			  APIDEMO_COUNT(other_menu));
		return;
	}
	if (action == FPLINUX_UI_MENU_CANCELLED) {
		snprintf(menu_demo.result, sizeof(menu_demo.result), "%s",
			 _("Cancelled: the callback received "
			   "FPLINUX_UI_MENU_CANCELLED."));
	} else {
		const char *text = "?";

		for (size_t i = 0; i < APIDEMO_COUNT(first_menu); i++)
			if ((int)first_menu[i].action == action)
				text = _(first_menu[i].text);
		for (size_t i = 0; i < APIDEMO_COUNT(other_menu); i++)
			if ((int)other_menu[i].action == action)
				text = _(other_menu[i].text);
		snprintf(menu_demo.result, sizeof(menu_demo.result),
			 _("Chosen: %s, action %d."), text, action);
	}
	show_menu_status();
}

/* The timed variant: close the menu if it is still open. */
static void menu_expired(lv_timer_t *timer)
{
	(void)timer;
	stop_menu_timer();
	if (!fplinux_ui_menu_is_open(menu_demo.app->ui))
		return;
	/* Closes without calling the callback. */
	fplinux_ui_menu_close(menu_demo.app->ui);
	snprintf(menu_demo.result, sizeof(menu_demo.result), "%s",
		 _("Closed by fplinux_ui_menu_close(): no callback."));
	show_menu_status();
}

/* The texts are copied and the icons decoded when the menu opens. */
static void open_menu(const char *title, const struct menu_entry *entries,
		      size_t count)
{
	struct apidemo *app = menu_demo.app;
	struct fplinux_ui_menu_item items[APIDEMO_COUNT(first_menu)];
	bool again;
	int error;

	if (count > APIDEMO_COUNT(items))
		count = APIDEMO_COUNT(items);
	for (size_t i = 0; i < count; i++)
		items[i] = (struct fplinux_ui_menu_item){
			.action = (int)entries[i].action,
			.text = _(entries[i].text),
			.icon = entries[i].glyph ?
					apidemo_glyph(app, entries[i].glyph) :
					NULL,
		};
	if (!fplinux_ui_menu_open(app->ui, title, items, count, menu_chosen,
				  NULL)) {
		apidemo_error(app, "fplinux_ui_menu_open");
		return;
	}
	/* Only one menu is open at a time: a second open fails. */
	again = fplinux_ui_menu_open(app->ui, title, items, count, menu_chosen,
				     NULL);
	error = errno;
	snprintf(menu_demo.busy, sizeof(menu_demo.busy),
		 _("While it was open, fplinux_ui_menu_is_open() returned %s "
		   "and a second open %s."),
		 fplinux_ui_menu_is_open(app->ui) ? "true" : "false",
		 again ? "true" : apidemo_errno_name(error));
	if (menu_demo.timed) {
		menu_demo.timer = lv_timer_create(menu_expired, MENU_MS, NULL);
		if (menu_demo.timer && !app->visible)
			lv_timer_pause(menu_demo.timer);
	}
}

static void menu_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *canvas, int command_id,
			 void *context)
{
	(void)canvas;
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_OPTIONS)
		open_menu(_("Options"), first_menu, APIDEMO_COUNT(first_menu));
}

/* The status may not fit: Up and Down scroll it. */
static void menu_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		     const struct fplinux_ui_key *key, void *context)
{
	(void)ui;
	(void)canvas;
	(void)context;
	if (menu_demo.scroll)
		fplinux_ui_scroll_key(menu_demo.scroll, key);
}

/* The canvas went with its screen: forget its objects and the timer. */
static void menu_canvas_deleted(lv_event_t *event)
{
	(void)event;
	stop_menu_timer();
	menu_demo.status = NULL;
	menu_demo.scroll = NULL;
}

/*
 * A scrolling text that fills a canvas below its title band: margin at the
 * left, gutter at the right for the scroll indicator, spacing above and
 * below. Returns the label; *scroll receives the container.
 */
static lv_obj_t *canvas_text(struct apidemo *app, lv_obj_t *area,
			     lv_obj_t **scroll)
{
	lv_obj_t *box = lv_obj_create(area);
	lv_obj_t *label;

	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, lv_pct(100), lv_pct(100));
	lv_obj_set_style_pad_left(box, app->metrics->margin, 0);
	lv_obj_set_style_pad_right(box, app->metrics->gutter, 0);
	lv_obj_set_style_pad_ver(box, app->metrics->spacing, 0);
	apidemo_style_scroll(app, box);
	label = lv_label_create(box);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, app->theme->font, 0);
	lv_obj_set_style_text_color(label, app->theme->foreground, 0);
	*scroll = box;
	return label;
}

static void open_canvas_menu(struct apidemo *app,
			     const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command options = {
		.id = COMMAND_OPTIONS,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Options"),
	};
	struct fplinux_ui_screen *canvas =
		fplinux_ui_canvas_new(app->ui, _(entry->title));

	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	stop_menu_timer();
	menu_demo.app = app;
	menu_demo.timed = entry->variant;
	menu_demo.status = canvas_text(app, fplinux_ui_canvas_object(canvas),
				       &menu_demo.scroll);
	lv_obj_add_event_cb(menu_demo.scroll, menu_canvas_deleted,
			    LV_EVENT_DELETE, NULL);
	snprintf(menu_demo.result, sizeof(menu_demo.result), "%s",
		 _("Nothing chosen yet."));
	menu_demo.busy[0] = '\0';
	show_menu_status();
	apidemo_add_back(canvas);
	fplinux_ui_screen_add_command(canvas, &options);
	fplinux_ui_screen_set_listener(canvas, menu_command, NULL, app);
	fplinux_ui_canvas_set_key_handler(canvas, menu_key, NULL);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
}

static const struct apidemo_entry entries[] = {
	{ N_("Command types"),
	  "FPLINUX_UI_COMMAND_* · label NULL · fplinux_ui_screen_add_command",
	  open_types, 0 },
	{ N_("Options menu with icons"),
	  "fplinux_ui_command.icon · fplinux_ui_glyph", open_icons, 0 },
	{ N_("Priorities and ties"), "fplinux_ui_command.priority", open_ranked,
	  0 },
	{ N_("Left command that fits"), "FPLINUX_UI_COMMAND_SCREEN · left key",
	  open_left, 0 },
	{ N_("Long left command"), "FPLINUX_UI_COMMAND_SCREEN · into Options",
	  open_left, 1 },
	{ N_("Help command"), "FPLINUX_UI_COMMAND_HELP", open_help, 0 },
	{ N_("Relabel a command"),
	  "fplinux_ui_screen_add_command · the same id", open_relabel, 0 },
	{ N_("Add and remove"),
	  "fplinux_ui_screen_remove_command · fplinux_ui_screen_set_title",
	  open_added, 0 },
	{ N_("Refused commands"), "fplinux_ui_screen_add_command · EINVAL",
	  open_errors, 0 },
	{ N_("Menu on a canvas"), "fplinux_ui_menu_open · _is_open · EBUSY",
	  open_canvas_menu, 0 },
	{ N_("Timed menu"), "fplinux_ui_menu_close · fplinux_ui_menu_is_open",
	  open_canvas_menu, 1 },
};

/* The timed menu waits while the application is hidden. */
static void commands_visibility(struct apidemo *app, bool visible)
{
	(void)app;
	if (!menu_demo.timer)
		return;
	if (visible)
		lv_timer_resume(menu_demo.timer);
	else
		lv_timer_pause(menu_demo.timer);
}

static void commands_cleanup(struct apidemo *app)
{
	(void)app;
	stop_menu_timer();
}

const struct apidemo_category apidemo_commands = {
	.title = N_("Commands and menus"),
	.icon = APIDEMO_IMAGE_DIR "commands-symbolic.png",
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = commands_visibility,
	.cleanup = commands_cleanup,
};
