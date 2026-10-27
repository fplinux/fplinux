/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * Trace backend of the screen natives, for tests of the look and feel in
 * the headless runner (FPLINUX_PM_LF_TRACE=1): a model of the runtime's
 * screen stack with Lists and Alerts, which prints every call as a "[lf] "
 * line on stdout. It starts like the app: the suite screen with the busy
 * "Starting" alert over it. The soft keys are assigned with the runtime's
 * rule (fplinux-ui-screen.h); an alert command removes the alert before
 * its listener runs, as the runtime does.
 *
 * FPLINUX_PM_LF_SCRIPT plays key presses: steps separated by ';', each
 * "<ms>:<action>", the delay counted from the previous step:
 *   dump          print the stack and the top screen's soft keys
 *   cmd:<label>   the top screen's command with that legend
 *   ok            OK: select (IMPLICIT), mark (EXCLUSIVE, MULTIPLE)
 *   focus:<row>   move the top list's focus
 *   wait:<title>  hold until Java has shown the top screen and it has that
 *                 title (20 s at most)
 *   rows:<count>  hold until the top list has that many rows
 * e.g. "500:dump;0:cmd:Settings;0:wait:Settings;0:dump;0:cmd:Exit".
 */
#include "fplinux-pm-lf-backend.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum fake_kind {
	FAKE_APP,
	FAKE_LIST,
	FAKE_ALERT,
};

struct fake_row {
	char *text;
	bool marked;
	bool icon;
};

struct fake_command {
	struct fplinux_ui_command command;
	char *label;
};

struct fake_screen {
	int number;
	enum fake_kind kind;
	int subkind;
	char *title;
	char *text;
	char *select_label;
	int progress_value;
	int progress_maximum;
	bool busy;
	struct fake_row *rows;
	size_t row_count;
	size_t focused;
	struct fake_command *commands;
	size_t command_count;
	fplinux_ui_command_cb on_command;
	fplinux_ui_change_cb on_change;
	void *context;
	void *data;
	struct fake_screen *below;
	bool stacked;
};

static const char *const kind_names[] = { "app", "list", "alert" };
static const char *const list_kinds[] = { "implicit", "exclusive", "multiple" };
static const char *const alert_kinds[] = { "info", "confirmation", "warning",
					   "error" };
static const char *const command_words[] = {
	"Back", "Cancel", "Exit", "Stop", "OK", "Item", "Screen", "Help"
};

static struct fake_screen *top;
/* The peer Java last showed (lf_trace_shown()), for wait steps. */
static const void *shown_screen;
static int last_number;
static bool started;
static char fake_ui;
static const char *script;

static struct fake_screen *fake(struct fplinux_ui_screen *screen)
{
	return (struct fake_screen *)screen;
}

static const struct fake_screen *
fake_const(const struct fplinux_ui_screen *screen)
{
	return (const struct fake_screen *)screen;
}

static char *copy(const char *text)
{
	return text ? strdup(text) : NULL;
}

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *format, ...)
{
	va_list arguments;

	fputs("[lf] ", stdout);
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	fputc('\n', stdout);
	fflush(stdout);
}

static const char *shown(const char *text)
{
	return text ? text : "(null)";
}

static struct fake_screen *new_screen(enum fake_kind kind, int subkind,
				      const char *title)
{
	struct fake_screen *screen = calloc(1, sizeof(*screen));

	if (!screen)
		return NULL;
	screen->number = ++last_number;
	screen->kind = kind;
	screen->subkind = subkind;
	screen->title = copy(title);
	screen->focused = SIZE_MAX;
	return screen;
}

static void delete_screen(struct fake_screen *screen)
{
	for (size_t i = 0; i < screen->row_count; ++i)
		free(screen->rows[i].text);
	for (size_t i = 0; i < screen->command_count; ++i)
		free(screen->commands[i].label);
	free(screen->rows);
	free(screen->commands);
	free(screen->title);
	free(screen->text);
	free(screen->select_label);
	free(screen);
}

/* The app's suite screen and its "Starting" alert, as fplinux-pm-app.c. */
static void start(void)
{
	struct fake_screen *suite;
	struct fake_screen *loading;

	if (started)
		return;
	started = true;
	suite = new_screen(FAKE_APP, 0, "suite");
	loading = new_screen(FAKE_ALERT, 0, "Starting");
	if (!suite || !loading) {
		free(suite);
		free(loading);
		return;
	}
	loading->busy = true;
	suite->stacked = true;
	loading->below = suite;
	loading->stacked = true;
	top = loading;
}

/* Stack */

static bool push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	struct fake_screen *s = fake(screen);

	(void)ui;
	if (!s || s->stacked) {
		errno = s ? EBUSY : EINVAL;
		return false;
	}
	start();
	s->below = top;
	s->stacked = true;
	top = s;
	say("push #%d", s->number);
	return true;
}

static bool replace(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	struct fake_screen *s = fake(screen);
	struct fake_screen *old;

	(void)ui;
	if (!s || s->stacked) {
		errno = s ? EBUSY : EINVAL;
		return false;
	}
	start();
	old = top;
	if (old) {
		top = old->below;
		old->stacked = false;
	}
	say("replace #%d by #%d", old ? old->number : 0, s->number);
	s->below = top;
	s->stacked = true;
	top = s;
	if (old)
		delete_screen(old);
	return true;
}

static bool pop(struct fplinux_ui *ui)
{
	struct fake_screen *old = top;

	(void)ui;
	if (!old || !old->below)
		return false;
	top = old->below;
	say("pop #%d", old->number);
	delete_screen(old);
	return true;
}

static struct fplinux_ui_screen *current(const struct fplinux_ui *ui)
{
	(void)ui;
	start();
	return (struct fplinux_ui_screen *)top;
}

/* Screens */

static void set_listener(struct fplinux_ui_screen *screen,
			 fplinux_ui_command_cb command,
			 fplinux_ui_change_cb change, void *context)
{
	fake(screen)->on_command = command;
	fake(screen)->on_change = change;
	fake(screen)->context = context;
}

static void set_data(struct fplinux_ui_screen *screen, void *data)
{
	fake(screen)->data = data;
}

static void *get_data(const struct fplinux_ui_screen *screen)
{
	return fake_const(screen)->data;
}

static bool set_title(struct fplinux_ui_screen *screen, const char *title)
{
	struct fake_screen *s = fake(screen);

	free(s->title);
	s->title = copy(title);
	say("title #%d \"%s\"", s->number, shown(title));
	return true;
}

static bool valid_label(const char *label)
{
	size_t length = 0;

	for (; label[length]; ++length)
		if ((unsigned char)label[length] < 0x20 ||
		    label[length] == 0x7f)
			return false;
	return length < 128;
}

static size_t find_command(const struct fake_screen *s, int id)
{
	for (size_t i = 0; i < s->command_count; ++i)
		if (s->commands[i].command.id == id)
			return i;
	return SIZE_MAX;
}

static void remove_command(struct fplinux_ui_screen *screen, int id)
{
	struct fake_screen *s = fake(screen);
	size_t at = find_command(s, id);

	if (at == SIZE_MAX)
		return;
	free(s->commands[at].label);
	memmove(&s->commands[at], &s->commands[at + 1],
		(s->command_count - at - 1) * sizeof(*s->commands));
	--s->command_count;
}

static bool add_command(struct fplinux_ui_screen *screen,
			const struct fplinux_ui_command *command)
{
	struct fake_screen *s = fake(screen);
	struct fake_command *commands;
	char *label = NULL;

	if ((unsigned int)command->type > FPLINUX_UI_COMMAND_HELP ||
	    (command->label && !valid_label(command->label)) ||
	    (!command->label && (command->type == FPLINUX_UI_COMMAND_ITEM ||
				 command->type == FPLINUX_UI_COMMAND_SCREEN))) {
		say("command refused #%d id=%d", s->number, command->id);
		return false;
	}
	if (command->label && !(label = strdup(command->label)))
		return false;
	remove_command(screen, command->id);
	commands = realloc(s->commands,
			   (s->command_count + 1) * sizeof(*s->commands));
	if (!commands) {
		free(label);
		return false;
	}
	s->commands = commands;
	s->commands[s->command_count].command = *command;
	s->commands[s->command_count].command.label = label;
	s->commands[s->command_count].label = label;
	++s->command_count;
	say("command #%d id=%d %s \"%s\" priority=%d", s->number, command->id,
	    command_words[command->type], shown(label), command->priority);
	return true;
}

static bool content_size(const struct fplinux_ui_screen *screen, int *width,
			 int *height)
{
	(void)screen;
	(void)width;
	(void)height;
	return false;
}

static size_t command_item(const struct fplinux_ui_screen *screen)
{
	(void)screen;
	return SIZE_MAX;
}

/* Lists */

static struct fplinux_ui_screen *list_new(struct fplinux_ui *ui,
					  const char *title,
					  enum fplinux_ui_list_kind kind)
{
	struct fake_screen *s = new_screen(FAKE_LIST, (int)kind, title);

	(void)ui;
	if (s)
		say("list #%d %s \"%s\"", s->number, list_kinds[kind],
		    shown(title));
	return (struct fplinux_ui_screen *)s;
}

static size_t list_insert(struct fplinux_ui_screen *list, size_t index,
			  const char *text, const char *detail)
{
	struct fake_screen *s = fake(list);
	struct fake_row *rows;

	(void)detail;
	if (index > s->row_count)
		return SIZE_MAX;
	rows = realloc(s->rows, (s->row_count + 1) * sizeof(*rows));
	if (!rows)
		return SIZE_MAX;
	s->rows = rows;
	memmove(&rows[index + 1], &rows[index],
		(s->row_count - index) * sizeof(*rows));
	rows[index] = (struct fake_row){ .text = copy(text) };
	++s->row_count;
	if (s->focused == SIZE_MAX)
		s->focused = 0;
	else if (index <= s->focused)
		++s->focused;
	say("insert #%d %zu \"%s\"", s->number, index, shown(text));
	return index;
}

static bool list_delete(struct fplinux_ui_screen *list, size_t index)
{
	struct fake_screen *s = fake(list);
	bool marked;

	if (index >= s->row_count)
		return false;
	marked = s->rows[index].marked;
	free(s->rows[index].text);
	memmove(&s->rows[index], &s->rows[index + 1],
		(s->row_count - index - 1) * sizeof(*s->rows));
	--s->row_count;
	if (!s->row_count)
		s->focused = SIZE_MAX;
	else if (index < s->focused || s->focused >= s->row_count)
		--s->focused;
	if (marked && s->subkind == FPLINUX_UI_LIST_EXCLUSIVE &&
	    s->focused != SIZE_MAX)
		s->rows[s->focused].marked = true;
	say("delete #%d %zu", s->number, index);
	return true;
}

static void list_delete_all(struct fplinux_ui_screen *list)
{
	struct fake_screen *s = fake(list);

	for (size_t i = 0; i < s->row_count; ++i)
		free(s->rows[i].text);
	s->row_count = 0;
	s->focused = SIZE_MAX;
	say("delete all #%d", s->number);
}

static bool list_set(struct fplinux_ui_screen *list, size_t index,
		     const char *text, const char *detail)
{
	struct fake_screen *s = fake(list);

	(void)detail;
	if (index >= s->row_count)
		return false;
	free(s->rows[index].text);
	s->rows[index].text = copy(text);
	say("set #%d %zu \"%s\"", s->number, index, shown(text));
	return true;
}

/* An icon line carries the pixels, so that a replay can draw them. */
static bool list_set_icon(struct fplinux_ui_screen *list, size_t index,
			  const void *src, enum fplinux_ui_list_icon_kind kind)
{
	struct fake_screen *s = fake(list);
	const lv_image_dsc_t *image = src;
	size_t bytes;

	(void)kind;
	if (index >= s->row_count)
		return false;
	if (!image) {
		if (s->rows[index].icon)
			say("icon #%d %zu off", s->number, index);
		s->rows[index].icon = false;
		return true;
	}
	s->rows[index].icon = true;
	bytes = image->data_size;
	printf("[lf] icon #%d %zu %ux%u %s ", s->number, index,
	       (unsigned int)image->header.w, (unsigned int)image->header.h,
	       image->header.cf == LV_COLOR_FORMAT_RGB565A8 ? "rgb565a8" :
							      "rgb565");
	for (size_t i = 0; i < bytes; ++i)
		printf("%02x", image->data[i]);
	fputc('\n', stdout);
	fflush(stdout);
	return true;
}

static size_t list_size(const struct fplinux_ui_screen *list)
{
	return fake_const(list)->row_count;
}

static size_t list_get_focused(const struct fplinux_ui_screen *list)
{
	return fake_const(list)->focused;
}

static void list_set_focused(struct fplinux_ui_screen *list, size_t index)
{
	struct fake_screen *s = fake(list);

	if (index < s->row_count && index != s->focused) {
		s->focused = index;
		say("focus #%d %zu", s->number, index);
	}
}

static bool list_is_selected(const struct fplinux_ui_screen *list, size_t index)
{
	const struct fake_screen *s = fake_const(list);

	return index < s->row_count && s->rows[index].marked;
}

static void list_set_selected(struct fplinux_ui_screen *list, size_t index,
			      bool selected)
{
	struct fake_screen *s = fake(list);

	if (index >= s->row_count ||
	    (s->subkind == FPLINUX_UI_LIST_EXCLUSIVE && !selected))
		return;
	if (s->subkind == FPLINUX_UI_LIST_EXCLUSIVE)
		for (size_t i = 0; i < s->row_count; ++i)
			s->rows[i].marked = false;
	if (s->rows[index].marked != selected)
		say("mark #%d %zu %s", s->number, index,
		    selected ? "on" : "off");
	s->rows[index].marked = selected;
}

static bool list_set_select_label(struct fplinux_ui_screen *list,
				  const char *label)
{
	struct fake_screen *s = fake(list);

	free(s->select_label);
	s->select_label = copy(label);
	say("select label #%d \"%s\"", s->number, shown(label));
	return true;
}

/* Alerts */

static struct fplinux_ui_screen *alert_new(struct fplinux_ui *ui,
					   enum fplinux_ui_alert_kind kind,
					   const char *title, const char *text,
					   unsigned int timeout_ms)
{
	struct fake_screen *s = new_screen(FAKE_ALERT, (int)kind, title);

	(void)ui;
	if (!s)
		return NULL;
	s->text = copy(text);
	say("alert #%d %s \"%s\" \"%s\" timeout=%u", s->number,
	    alert_kinds[kind], shown(title), shown(text), timeout_ms);
	return (struct fplinux_ui_screen *)s;
}

static bool alert_set_text(struct fplinux_ui_screen *alert, const char *text)
{
	struct fake_screen *s = fake(alert);

	free(s->text);
	s->text = copy(text);
	say("alert text #%d \"%s\"", s->number, shown(text));
	return true;
}

static bool alert_set_progress(struct fplinux_ui_screen *alert, int value,
			       int maximum)
{
	struct fake_screen *s = fake(alert);

	s->busy = true;
	s->progress_value = value;
	s->progress_maximum = maximum;
	say("progress #%d %d/%d", s->number, value, maximum);
	return true;
}

static const struct lf_backend trace_backend = {
	.ui = (struct fplinux_ui *)&fake_ui,
	.replace = replace,
	.push = push,
	.pop = pop,
	.current = current,
	.set_listener = set_listener,
	.set_data = set_data,
	.get_data = get_data,
	.set_title = set_title,
	.add_command = add_command,
	.remove_command = remove_command,
	.content_size = content_size,
	.command_item = command_item,
	.list_new = list_new,
	.list_insert = list_insert,
	.list_delete = list_delete,
	.list_delete_all = list_delete_all,
	.list_set = list_set,
	.list_set_icon = list_set_icon,
	.list_size = list_size,
	.list_get_focused = list_get_focused,
	.list_set_focused = list_set_focused,
	.list_is_selected = list_is_selected,
	.list_set_selected = list_set_selected,
	.list_set_select_label = list_set_select_label,
	.alert_new = alert_new,
	.alert_set_text = alert_set_text,
	.alert_set_progress = alert_set_progress,
};

void lf_trace_shown(const struct fplinux_ui_screen *screen)
{
	shown_screen = screen;
}

bool lf_trace_enabled(void)
{
	const char *value = getenv("FPLINUX_PM_LF_TRACE");

	return value && value[0] && strcmp(value, "0");
}

/* The app's screens come first, so screen numbers do not depend on timing. */
const struct lf_backend *lf_trace_backend(void)
{
	start();
	return &trace_backend;
}

/* Soft keys, by the runtime's rule */

static bool back_role(int type)
{
	return type <= FPLINUX_UI_COMMAND_STOP;
}

static const char *legend(const struct fake_command *c)
{
	return c->label ? c->label : command_words[c->command.type];
}

/* The focused element's own action on OK, as the runtime's lists have. */
static const char *element_action(const struct fake_screen *s)
{
	if (s->kind != FAKE_LIST || !s->row_count)
		return NULL;
	if (s->subkind == FPLINUX_UI_LIST_MULTIPLE)
		return s->focused != SIZE_MAX && s->rows[s->focused].marked ?
			       "Unmark" :
			       "Mark";
	if (s->subkind == FPLINUX_UI_LIST_IMPLICIT && s->select_label)
		return s->select_label;
	return "Select";
}

/* The index order of the commands by priority, ties in order of addition. */
static void by_priority(const struct fake_screen *s, size_t *order)
{
	for (size_t i = 0; i < s->command_count; ++i) {
		size_t at = i;

		while (at && s->commands[order[at - 1]].command.priority >
				     s->commands[i].command.priority) {
			order[at] = order[at - 1];
			--at;
		}
		order[at] = i;
	}
}

struct keys {
	size_t right;
	size_t ok;
	size_t left;
	size_t menu[64];
	size_t menu_count;
};

static void assign(const struct fake_screen *s, struct keys *keys)
{
	size_t order[64];
	size_t count = s->command_count < 64 ? s->command_count : 64;
	bool acts = element_action(s) != NULL;
	bool help = false;

	*keys = (struct keys){ SIZE_MAX, SIZE_MAX, SIZE_MAX, { 0 }, 0 };
	by_priority(s, order);
	for (size_t i = 0; i < count; ++i) {
		int type = s->commands[order[i]].command.type;

		if (keys->right == SIZE_MAX && back_role(type))
			keys->right = order[i];
		else if (!acts && keys->ok == SIZE_MAX &&
			 type == FPLINUX_UI_COMMAND_OK)
			keys->ok = order[i];
		else
			keys->menu[keys->menu_count++] = order[i];
	}
	for (size_t i = 0; i < keys->menu_count; ++i)
		help = help || s->commands[keys->menu[i]].command.type ==
				       FPLINUX_UI_COMMAND_HELP;
	if (keys->menu_count == 1 && !help) {
		keys->left = keys->menu[0];
		keys->menu_count = 0;
	}
}

static void dump(void)
{
	char line[1024];
	size_t used = 0;
	struct fake_screen *chain[32];
	size_t depth = 0;
	struct keys keys;

	start();
	for (struct fake_screen *s = top; s && depth < 32; s = s->below)
		chain[depth++] = s;
	while (depth--) {
		struct fake_screen *s = chain[depth];

		used += (size_t)snprintf(line + used, sizeof(line) - used,
					 "%s#%d %s \"%s\"", used ? " | " : "",
					 s->number, kind_names[s->kind],
					 shown(s->title));
		if (used >= sizeof(line))
			break;
	}
	say("stack: %s", line);
	if (!top)
		return;
	if (top->kind == FAKE_LIST) {
		used = 0;
		line[0] = '\0';
		for (size_t i = 0; i < top->row_count && used < sizeof(line);
		     ++i)
			used += (size_t)snprintf(line + used,
						 sizeof(line) - used,
						 "%s%s%s%s", i ? ", " : "",
						 i == top->focused ? ">" : "",
						 top->rows[i].marked ? "*" : "",
						 shown(top->rows[i].text));
		say("rows #%d %s: %s", top->number, list_kinds[top->subkind],
		    line);
	} else if (top->kind == FAKE_ALERT) {
		say("alert #%d %s text=\"%s\"%s", top->number,
		    alert_kinds[top->subkind], shown(top->text),
		    top->busy ? " busy" : "");
	}
	assign(top, &keys);
	used = 0;
	line[0] = '\0';
	for (size_t i = 0; i < keys.menu_count && used < sizeof(line); ++i)
		used += (size_t)snprintf(line + used, sizeof(line) - used,
					 "%s%s", i ? ", " : "",
					 legend(&top->commands[keys.menu[i]]));
	say("keys #%d: left=%s%s%s%s ok=%s right=%s", top->number,
	    keys.left != SIZE_MAX ? legend(&top->commands[keys.left]) :
	    keys.menu_count	  ? "Options" :
				    "-",
	    keys.menu_count ? "[" : "", line, keys.menu_count ? "]" : "",
	    element_action(top) ? element_action(top) :
	    keys.ok != SIZE_MAX ? legend(&top->commands[keys.ok]) :
				  "-",
	    keys.right != SIZE_MAX ? legend(&top->commands[keys.right]) : "-");
}

/* Script */

/* Fires a command as the runtime does: an alert leaves the stack first. */
static void fire(struct fake_screen *s, int id)
{
	bool alert = s->kind == FAKE_ALERT;

	if (alert && top == s) {
		top = s->below;
		s->below = NULL;
		s->stacked = false;
	}
	if (s->on_command)
		s->on_command((struct fplinux_ui *)&fake_ui,
			      (struct fplinux_ui_screen *)s, id, s->context);
	if (alert && !s->stacked)
		delete_screen(s);
}

static void press_label(const char *label)
{
	struct fake_screen *s = top;

	for (size_t i = 0; s && i < s->command_count; ++i) {
		if (!strcmp(legend(&s->commands[i]), label)) {
			say("press \"%s\" on #%d", label, s->number);
			fire(s, s->commands[i].command.id);
			return;
		}
	}
	say("press \"%s\": no such command on #%d", label, s ? s->number : 0);
}

static void press_ok(void)
{
	struct fake_screen *s = top;
	struct keys keys;

	if (!s)
		return;
	if (s->kind == FAKE_LIST && s->focused != SIZE_MAX) {
		say("press OK on #%d row %zu", s->number, s->focused);
		if (s->subkind == FPLINUX_UI_LIST_IMPLICIT) {
			fire(s, FPLINUX_UI_SELECT_COMMAND);
			return;
		}
		list_set_selected((struct fplinux_ui_screen *)s, s->focused,
				  s->subkind == FPLINUX_UI_LIST_EXCLUSIVE ||
					  !s->rows[s->focused].marked);
		if (s->on_change)
			s->on_change((struct fplinux_ui *)&fake_ui,
				     (struct fplinux_ui_screen *)s, s->focused,
				     s->context);
		return;
	}
	assign(s, &keys);
	if (keys.ok != SIZE_MAX) {
		say("press OK on #%d", s->number);
		fire(s, s->commands[keys.ok].command.id);
	}
}

/* A wait step polls every WAIT_POLL_MS, at most WAIT_POLLS times. */
#define WAIT_POLL_MS 50
#define WAIT_POLLS 400

static int wait_polls;

/* The current step: "<ms>:<action>"; script points at it. */
int lf_trace_next_delay(void)
{
	char *end;
	long delay;

	if (!script)
		script = getenv("FPLINUX_PM_LF_SCRIPT");
	if (!script || !*script)
		return -1;
	if (wait_polls)
		return WAIT_POLL_MS;
	delay = strtol(script, &end, 10);
	if (end == script || *end != ':' || delay < 0) {
		say("script: bad step \"%s\"", script);
		script = "";
		return -1;
	}
	return delay > INT32_MAX ? INT32_MAX : (int)delay;
}

/* wait:<title> and rows:<count> hold the script until the top matches. */
static bool waiting(const char *step)
{
	bool met;

	if (!strncmp(step, "wait:", 5))
		met = top && top == shown_screen && top->title &&
		      !strcmp(top->title, step + 5);
	else if (!strncmp(step, "rows:", 5))
		met = top && top->kind == FAKE_LIST &&
		      top->row_count == (size_t)strtoul(step + 5, NULL, 10);
	else
		return false;
	if (!met && ++wait_polls <= WAIT_POLLS)
		return true;
	if (!met)
		say("script: %s timed out", step);
	wait_polls = 0;
	return false;
}

void lf_trace_run_step(void)
{
	char step[256];
	const char *action;
	size_t length;

	if (lf_trace_next_delay() < 0)
		return;
	action = strchr(script, ':') + 1;
	length = strcspn(action, ";");
	if (length >= sizeof(step))
		length = sizeof(step) - 1;
	memcpy(step, action, length);
	step[length] = '\0';
	start();
	if (waiting(step))
		return;
	script = action + strcspn(action, ";");
	if (*script == ';')
		++script;
	if (!strcmp(step, "dump")) {
		dump();
	} else if (!strncmp(step, "cmd:", 4)) {
		press_label(step + 4);
	} else if (!strcmp(step, "ok")) {
		press_ok();
	} else if (!strncmp(step, "focus:", 6)) {
		if (top && top->kind == FAKE_LIST)
			list_set_focused((struct fplinux_ui_screen *)top,
					 (size_t)strtoul(step + 6, NULL, 10));
	} else if (strncmp(step, "wait:", 5) && strncmp(step, "rows:", 5)) {
		say("script: unknown action \"%s\"", step);
	}
}
