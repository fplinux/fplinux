/* SPDX-License-Identifier: 0BSD */
/*
 * Text: the text box screen and the system text field on a canvas.
 *
 * A text box is a whole screen for one multi-line field: the runtime gives
 * it its keys, the New line legend on OK and Clear on the right soft key.
 * On a canvas the application creates the field itself and passes it the
 * keys; see the last demo.
 */
#include "apidemo.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

/*
 * Canvas fields can combine PASSWORD and UNEDITABLE with each base type.
 */
static const struct {
	enum fplinux_ui_text_constraint constraint;
	const char *name;
} types[] = {
	{ FPLINUX_UI_TEXT_ANY, "ANY" },
	{ FPLINUX_UI_TEXT_NUMERIC, "NUMERIC" },
	{ FPLINUX_UI_TEXT_DECIMAL, "DECIMAL" },
	{ FPLINUX_UI_TEXT_PHONE, "PHONE" },
	{ FPLINUX_UI_TEXT_ANY | FPLINUX_UI_TEXT_PASSWORD, "ANY | PASSWORD" },
	{ FPLINUX_UI_TEXT_NUMERIC | FPLINUX_UI_TEXT_PASSWORD,
	  "NUMERIC | PASSWORD" },
	{ FPLINUX_UI_TEXT_DECIMAL | FPLINUX_UI_TEXT_PASSWORD,
	  "DECIMAL | PASSWORD" },
	{ FPLINUX_UI_TEXT_PHONE | FPLINUX_UI_TEXT_PASSWORD,
	  "PHONE | PASSWORD" },
	{ FPLINUX_UI_TEXT_ANY | FPLINUX_UI_TEXT_UNEDITABLE,
	  "ANY | UNEDITABLE" },
	{ FPLINUX_UI_TEXT_NUMERIC | FPLINUX_UI_TEXT_UNEDITABLE,
	  "NUMERIC | UNEDITABLE" },
	{ FPLINUX_UI_TEXT_DECIMAL | FPLINUX_UI_TEXT_UNEDITABLE,
	  "DECIMAL | UNEDITABLE" },
	{ FPLINUX_UI_TEXT_PHONE | FPLINUX_UI_TEXT_UNEDITABLE,
	  "PHONE | UNEDITABLE" },
	{ FPLINUX_UI_TEXT_ANY | FPLINUX_UI_TEXT_PASSWORD |
		  FPLINUX_UI_TEXT_UNEDITABLE,
	  "ANY | PASSWORD | UNEDITABLE" },
	{ FPLINUX_UI_TEXT_NUMERIC | FPLINUX_UI_TEXT_PASSWORD |
		  FPLINUX_UI_TEXT_UNEDITABLE,
	  "NUMERIC | PASSWORD | UNEDITABLE" },
	{ FPLINUX_UI_TEXT_DECIMAL | FPLINUX_UI_TEXT_PASSWORD |
		  FPLINUX_UI_TEXT_UNEDITABLE,
	  "DECIMAL | PASSWORD | UNEDITABLE" },
	{ FPLINUX_UI_TEXT_PHONE | FPLINUX_UI_TEXT_PASSWORD |
		  FPLINUX_UI_TEXT_UNEDITABLE,
	  "PHONE | PASSWORD | UNEDITABLE" },
};

/* Characters, not bytes: the lead bytes of UTF-8. */
static unsigned int count_characters(const char *text)
{
	unsigned int count = 0;

	for (; text && *text; text++)
		count += ((unsigned char)*text & 0xc0) != 0x80;
	return count;
}

/* Text boxes */

enum {
	COMMAND_SHOW = 1,
	COMMAND_SAMPLE,
};

/* The limit of the box with a counter. */
#define BOX_LIMIT 160
/* The string belongs to the box and changes with its text: copy to keep. */
static void show_box_text(struct apidemo *app, struct fplinux_ui_screen *box)
{
	const char *text = fplinux_ui_text_box_get(box);
	unsigned int count = count_characters(text);

	apidemo_showf(app, _("Text"),
		      ngettext("%1$u character:\n%2$s",
			       "%1$u characters:\n%2$s", count),
		      count, text ? text : "");
}

static void box_command(struct fplinux_ui *ui, struct fplinux_ui_screen *box,
			int command_id, void *context)
{
	struct apidemo *app = context;

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_SHOW:
		show_box_text(app, box);
		break;
	case COMMAND_SAMPLE:
		if (!fplinux_ui_text_box_set(
			    box, _("A text box takes several lines. OK "
				   "starts a new one.")))
			apidemo_error(app, "fplinux_ui_text_box_set");
		break;
	default:
		break;
	}
}

/* Adds SCREEN commands with their domain glyphs, in priority order. */
static void add_screen_commands(struct apidemo *app,
				struct fplinux_ui_screen *screen,
				const int *ids, const char *const *labels,
				const char *const *glyphs, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const struct fplinux_ui_command command = {
			.id = ids[i],
			.type = FPLINUX_UI_COMMAND_SCREEN,
			.label = _(labels[i]),
			.priority = (int)i,
			.icon = apidemo_glyph(app, glyphs[i]),
		};

		fplinux_ui_screen_add_command(screen, &command);
	}
}

/* The editor has a 160-character counter and the runtime's Clear/Back. */
static void open_box(struct apidemo *app, const struct apidemo_entry *entry)
{
	static const int ids[] = { COMMAND_SHOW, COMMAND_SAMPLE };
	static const char *const labels[] = {
		N_("Show"),
		N_("Insert sample"),
	};
	static const char *const glyphs[] = { "view", "add" };
	struct fplinux_ui_screen *box = fplinux_ui_text_box_new(
		app->ui, _(entry->title), NULL, BOX_LIMIT);

	if (!box) {
		apidemo_error(app, "fplinux_ui_text_box_new");
		return;
	}
	apidemo_add_back(box);
	add_screen_commands(app, box, ids, labels, glyphs, APIDEMO_COUNT(ids));
	fplinux_ui_screen_set_listener(box, box_command, NULL, app);
	apidemo_push(app, box, "fplinux_ui_text_box_new");
}

/*
 * UNEDITABLE makes the box a viewer: no caret, mode or counter, Up and Down
 * scroll by a line, Left and Right by a page, and the right soft key keeps
 * Back.
 */
static void open_viewer(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *box = fplinux_ui_text_box_new(
		app->ui, _(entry->title),
		_("A read-only text box shows a long text, such as a licence "
		  "or a message, without a caret, a mode or a counter.\n\n"
		  "Up and Down scroll it by a line, Left and Right by a page. "
		  "The scroll indicator at the right shows where you are.\n\n"
		  "The right soft key stays Back: there is no Clear, because "
		  "nothing can be typed. OK starts no new line.\n\n"
		  "The application can still replace the text with "
		  "fplinux_ui_text_box_set(), for example when a new message "
		  "arrives.\n\n"
		  "Give the box FPLINUX_UI_TEXT_UNEDITABLE with "
		  "fplinux_ui_text_box_set_constraint(), added with | to any "
		  "type.\n\n"
		  "This is the end of the text."),
		0);

	if (!box) {
		apidemo_error(app, "fplinux_ui_text_box_new");
		return;
	}
	apidemo_add_back(box);
	fplinux_ui_screen_set_listener(box, apidemo_back_listener, NULL, app);
	apidemo_push(app, box, "fplinux_ui_text_box_new");
	if (!fplinux_ui_text_box_set_constraint(
		    box, FPLINUX_UI_TEXT_ANY | FPLINUX_UI_TEXT_UNEDITABLE))
		apidemo_error(app, "fplinux_ui_text_box_set_constraint");
}

/* Text fields on a canvas */

/*
 * The screen stack owns the soft keys of a canvas: a command on the right
 * soft key fires before the key handler could pass the key to a field. A
 * form or a text box shows Clear itself while its field holds text; on a
 * canvas the application does it with a command whose id stays the same
 * while its type and label follow the field:
 *
 *   the field is empty     BACK, the runtime's Back: leave the screen;
 *   the field holds text   CANCEL labelled Clear, Back in Options;
 *   the symbol grid is up  CANCEL with the runtime's Cancel.
 *
 * The canvas filter gives physical keys to the field before commands. It
 * owns Clear through its release, even if the hold empties the field and
 * the legend becomes Back. Commands chosen in Options use a synthetic
 * press and release: Clear deletes a character, Cancel closes the grid,
 * and OK inserts a newline or the selected symbol.
 */
enum {
	COMMAND_RIGHT = 1,
	COMMAND_OK,
	COMMAND_NEXT,
	COMMAND_FIELD_TYPE,
	COMMAND_FIELD_SAMPLE,
	COMMAND_FIELD_SHOW,
	COMMAND_SECOND,
	COMMAND_FIELD_CLEAR,
	COMMAND_FIELD_LIMIT,
};

enum right_key {
	RIGHT_NONE,
	RIGHT_BACK,
	RIGHT_CLEAR,
	RIGHT_PICKER,
};

/* The optional limit of either field, shown as its counter. */
#define LINE_LIMIT 24
#define FIELD_COUNT 2

static struct edit_state {
	struct apidemo *app;
	struct fplinux_ui_screen *canvas;
	/* The scrolling column of items, each a caption and a field. */
	lv_obj_t *column;
	lv_obj_t *items[FIELD_COUNT];
	lv_obj_t *captions[FIELD_COUNT];
	/* fields[1] is NULL while the second field is removed. */
	struct fplinux_ui_text *fields[FIELD_COUNT];
	size_t types[FIELD_COUNT];
	size_t limits[FIELD_COUNT];
	size_t focus;
	/* The commands the canvas has now, to change only what changed. */
	enum right_key right;
	const char *ok;
	bool options;
	bool second;
} edit;

static void caption_field(size_t index)
{
	char text[128];

	if (edit.limits[index])
		snprintf(text, sizeof(text),
			 index ? _("Several lines, up to 24 characters: %s") :
				 _("One line, up to 24 characters: %s"),
			 types[edit.types[index]].name);
	else
		snprintf(text, sizeof(text),
			 index ? _("Several lines, no limit: %s") :
				 _("One line, no limit: %s"),
			 types[edit.types[index]].name);
	lv_label_set_text(edit.captions[index], text);
}

/* Gives the field a press and a release of a phone key. */
static void feed_key(struct fplinux_ui_text *field, unsigned int code)
{
	struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = code,
		.pressed = true,
		.time_ms = fplinux_ui_now_ms(),
	};

	fplinux_ui_text_key(field, &key);
	key.pressed = false;
	fplinux_ui_text_key(field, &key);
}

static void add_screen_command(int id, const char *label, int priority,
			       const char *glyph)
{
	const struct fplinux_ui_command command = {
		.id = id,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = label,
		.priority = priority,
		.icon = apidemo_glyph(edit.app, glyph),
	};

	fplinux_ui_screen_add_command(edit.canvas, &command);
}

/* Matches the commands to the focused field; called on every change. */
static void sync_commands(void)
{
	struct fplinux_ui_text *field = edit.fields[edit.focus];
	enum fplinux_ui_text_constraint constraint =
		types[edit.types[edit.focus]].constraint;
	bool picker = fplinux_ui_text_is_picker_open(field);
	bool read_only = constraint & FPLINUX_UI_TEXT_UNEDITABLE;
	bool multiline_text = edit.focus &&
			      (constraint == FPLINUX_UI_TEXT_ANY ||
			       constraint == (FPLINUX_UI_TEXT_ANY |
					      FPLINUX_UI_TEXT_PASSWORD));
	enum right_key right = picker ? RIGHT_PICKER :
			       read_only || fplinux_ui_text_is_empty(field) ?
					RIGHT_BACK :
					RIGHT_CLEAR;
	const char *ok = picker		? _("Insert") :
			 multiline_text ? _("New line") :
					  NULL;

	if (right != edit.right) {
		struct fplinux_ui_command command = {
			.id = COMMAND_RIGHT,
			.type = right == RIGHT_BACK ? FPLINUX_UI_COMMAND_BACK :
						      FPLINUX_UI_COMMAND_CANCEL,
			/* NULL: the runtime's Back or Cancel. */
			.label = right == RIGHT_CLEAR ? _("Clear") : NULL,
			/* Clear/Cancel keeps RSK; a separate Back goes to Options. */
			.priority = -2,
			.icon = right == RIGHT_CLEAR ?
					apidemo_glyph(edit.app, "backspace") :
					NULL,
		};
		const struct fplinux_ui_command back = {
			.id = APIDEMO_COMMAND_BACK,
			.type = FPLINUX_UI_COMMAND_BACK,
		};

		/* The same id: the command is replaced, not added. */
		fplinux_ui_screen_add_command(edit.canvas, &command);
		if (right == RIGHT_CLEAR)
			fplinux_ui_screen_add_command(edit.canvas, &back);
		else
			fplinux_ui_screen_remove_command(edit.canvas,
							 APIDEMO_COMMAND_BACK);
		edit.right = right;
	}
	if (ok != edit.ok) {
		const struct fplinux_ui_command command = {
			.id = COMMAND_OK,
			.type = FPLINUX_UI_COMMAND_OK,
			.label = ok,
			.icon = apidemo_glyph(edit.app,
					      picker ? "add" : "enter"),
		};

		if (ok)
			fplinux_ui_screen_add_command(edit.canvas, &command);
		else
			fplinux_ui_screen_remove_command(edit.canvas,
							 COMMAND_OK);
		edit.ok = ok;
	}
	/* The grid takes the keys: the options menu waits until it closes. */
	if (picker == edit.options || edit.second != !!edit.fields[1]) {
		edit.options = !picker;
		edit.second = edit.fields[1] != NULL;
		fplinux_ui_screen_remove_command(edit.canvas, COMMAND_NEXT);
		fplinux_ui_screen_remove_command(edit.canvas,
						 COMMAND_FIELD_TYPE);
		fplinux_ui_screen_remove_command(edit.canvas,
						 COMMAND_FIELD_SAMPLE);
		fplinux_ui_screen_remove_command(edit.canvas,
						 COMMAND_FIELD_SHOW);
		fplinux_ui_screen_remove_command(edit.canvas, COMMAND_SECOND);
		fplinux_ui_screen_remove_command(edit.canvas,
						 COMMAND_FIELD_CLEAR);
		fplinux_ui_screen_remove_command(edit.canvas,
						 COMMAND_FIELD_LIMIT);
		if (!edit.options)
			return;
		if (edit.second)
			add_screen_command(COMMAND_NEXT, _("Next field"), 0,
					   "next");
		add_screen_command(COMMAND_FIELD_TYPE, _("Next type"), 1,
				   "keyboard");
		add_screen_command(COMMAND_FIELD_SAMPLE, _("Insert sample"), 2,
				   "add");
		add_screen_command(COMMAND_FIELD_SHOW, _("Show"), 3, "view");
		add_screen_command(COMMAND_SECOND,
				   edit.second ? _("Remove second field") :
						 _("Add second field"),
				   4, edit.second ? "delete" : "add");
		add_screen_command(COMMAND_FIELD_CLEAR, _("Clear text"), 5,
				   "backspace");
		add_screen_command(COMMAND_FIELD_LIMIT, _("Toggle limit"), 6,
				   "text-size");
	}
}

/* Runs after any change of a field's text, caret, mode or symbol grid. */
static void field_changed(struct fplinux_ui_text *field, void *context)
{
	(void)context;
	if (field == edit.fields[edit.focus])
		sync_commands();
}

/*
 * Scrolls the focused item, caption and field, into view, and then at least
 * the whole frame: its parent, the item, does not scroll, the column does.
 */
static void reveal_focus(void)
{
	lv_obj_scroll_to_view(edit.items[edit.focus], LV_ANIM_OFF);
	lv_obj_scroll_to_view_recursive(
		fplinux_ui_text_object(edit.fields[edit.focus]), LV_ANIM_OFF);
}

/* An unfocused field commits pending input and takes no keys. */
static void focus_field(size_t index)
{
	if (index >= FIELD_COUNT || !edit.fields[index])
		return;
	fplinux_ui_text_set_focused(edit.fields[edit.focus], false);
	edit.focus = index;
	fplinux_ui_text_set_focused(edit.fields[index], true);
	reveal_focus();
	sync_commands();
}

/* A sample that suits the type of the field. */
static void insert_sample(struct fplinux_ui_text *field, size_t type)
{
	const char *sample;

	switch (types[type].constraint &
		~(FPLINUX_UI_TEXT_PASSWORD | FPLINUX_UI_TEXT_UNEDITABLE)) {
	case FPLINUX_UI_TEXT_NUMERIC:
		sample = "-42";
		break;
	case FPLINUX_UI_TEXT_DECIMAL:
		/* A decimal field takes either separator. */
		sample = "3.14";
		break;
	case FPLINUX_UI_TEXT_PHONE:
		sample = "+15551234567";
		break;
	default:
		sample = _("Hello");
		break;
	}
	if (!fplinux_ui_text_set(field, sample))
		apidemo_error(edit.app, "fplinux_ui_text_set");
}

static void next_field_type(size_t index)
{
	size_t next = (edit.types[index] + 1) % APIDEMO_COUNT(types);

	if (!fplinux_ui_text_set_constraint(edit.fields[index],
					    types[next].constraint)) {
		apidemo_showf(edit.app, _("Type kept"),
			      _("%1$s: the text does not suit %2$s. Clear it "
				"and try again."),
			      apidemo_errno_name(errno), types[next].name);
		return;
	}
	edit.types[index] = next;
	/* A longer caption may push the frame down. */
	caption_field(index);
	reveal_focus();
	/* The type setter's callback ran before edit.types was updated. */
	sync_commands();
}

static void show_field(struct fplinux_ui_text *field)
{
	apidemo_showf(edit.app, _("Text"),
		      _("Text: “%1$s”\nEmpty: %2$s\nMode: %3$s"),
		      fplinux_ui_text_get(field),
		      fplinux_ui_text_is_empty(field) ? "true" : "false",
		      fplinux_ui_text_indicator(field));
}

static struct fplinux_ui_text *create_field(size_t index);
static void toggle_limit(void);

/*
 * fplinux_ui_text_delete() removes a field while its parent stays; a
 * field also goes with its parent, as these do with the canvas.
 */
static void toggle_second(void)
{
	if (edit.fields[1]) {
		if (edit.focus)
			focus_field(0);
		fplinux_ui_text_delete(edit.fields[1]);
		edit.fields[1] = NULL;
		lv_obj_set_hidden(edit.items[1], true);
	} else if (create_field(1)) {
		lv_obj_set_hidden(edit.items[1], false);
		focus_field(1);
		return;
	} else {
		apidemo_error(edit.app, "fplinux_ui_text_create");
	}
	sync_commands();
}

static void edit_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *canvas, int command_id,
			 void *context)
{
	struct fplinux_ui_text *field = edit.fields[edit.focus];

	(void)canvas;
	(void)context;
	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_RIGHT:
		if (edit.right == RIGHT_BACK)
			fplinux_ui_go_back(ui);
		else
			feed_key(field, KEY_F14);
		break;
	case COMMAND_OK:
		feed_key(field, KEY_OK);
		break;
	case COMMAND_NEXT:
		focus_field(edit.focus ? 0 : 1);
		break;
	case COMMAND_FIELD_TYPE:
		next_field_type(edit.focus);
		break;
	case COMMAND_FIELD_SAMPLE:
		insert_sample(field, edit.types[edit.focus]);
		break;
	case COMMAND_FIELD_SHOW:
		show_field(field);
		break;
	case COMMAND_SECOND:
		toggle_second();
		break;
	case COMMAND_FIELD_CLEAR:
		if (!fplinux_ui_text_set(field, ""))
			apidemo_error(edit.app, "fplinux_ui_text_set");
		sync_commands();
		break;
	case COMMAND_FIELD_LIMIT:
		toggle_limit();
		break;
	default:
		break;
	}
}

/* Physical field input precedes commands, including held Clear. */
static bool edit_filter(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
			const struct fplinux_ui_key *key, void *context)
{
	(void)ui;
	(void)canvas;
	(void)context;
	return fplinux_ui_text_key(edit.fields[edit.focus], key);
}

/* Up and Down that the field leaves move the focus. */
static void edit_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		     const struct fplinux_ui_key *key, void *context)
{
	(void)ui;
	(void)canvas;
	(void)context;
	if (!key->pressed)
		return;
	if (key->code == KEY_UP)
		focus_field(0);
	else if (key->code == KEY_DOWN)
		focus_field(1);
}

/* The canvas went with its screen, and the fields with it. */
static void edit_deleted(lv_event_t *event)
{
	(void)event;
	edit.canvas = NULL;
	edit.column = NULL;
	for (size_t i = 0; i < FIELD_COUNT; i++) {
		edit.items[i] = NULL;
		edit.captions[i] = NULL;
		edit.fields[i] = NULL;
	}
}

/*
 * The field's frame goes below its caption and fills the item's width. A
 * frame sizes itself to its lines; the multi-line one may be no taller
 * than the canvas shows. NULL with errno when the field cannot be created.
 */
static struct fplinux_ui_text *new_field(size_t index, size_t max_chars)
{
	const struct fplinux_ui_metrics *metrics = edit.app->metrics;
	struct fplinux_ui_text *field;
	lv_obj_t *frame;
	int width;
	int height;

	if (!fplinux_ui_screen_get_content_size(edit.canvas, &width, &height))
		return NULL;
	field = fplinux_ui_text_create(edit.app->ui, edit.items[index],
				       index == 1, max_chars);
	if (!field)
		return NULL;
	frame = fplinux_ui_text_object(field);
	lv_obj_set_width(frame, lv_pct(100));
	lv_obj_set_style_max_height(frame, height - 2 * metrics->spacing, 0);
	/* A new field takes the focus: give it back until it is chosen. */
	fplinux_ui_text_set_focused(field, false);
	return field;
}

static struct fplinux_ui_text *create_field(size_t index)
{
	size_t limit = index ? 0 : LINE_LIMIT;
	struct fplinux_ui_text *field = new_field(index, limit);

	if (!field)
		return NULL;
	fplinux_ui_text_set_change_cb(field, field_changed, NULL);
	edit.fields[index] = field;
	edit.types[index] = 0;
	edit.limits[index] = limit;
	caption_field(index);
	return field;
}

/*
 * There is no max_chars setter: change the limit by recreating this field.
 * Configure the replacement first, with the old type and text, while the
 * old field still works. In particular, E2BIG must not truncate its text.
 * On success the same field stays focused; its caret moves to the end.
 */
static void toggle_limit(void)
{
	size_t index = edit.focus;
	size_t limit = edit.limits[index] ? 0 : LINE_LIMIT;
	struct fplinux_ui_text *old = edit.fields[index];
	struct fplinux_ui_text *field = new_field(index, limit);
	const char *call = "fplinux_ui_text_create";
	int error;

	if (!field) {
		apidemo_error(edit.app, call);
		return;
	}
	call = "fplinux_ui_text_set_constraint";
	if (!fplinux_ui_text_set_constraint(
		    field, types[edit.types[index]].constraint))
		goto refused;
	call = "fplinux_ui_text_set";
	/* Resolve the frame's size before the text scrolls to its new caret. */
	lv_obj_update_layout(fplinux_ui_text_object(field));
	/* The borrowed text stays valid: old is not deleted until this succeeds. */
	if (!fplinux_ui_text_set(field, fplinux_ui_text_get(old)))
		goto refused;
	edit.fields[index] = field;
	edit.limits[index] = limit;
	fplinux_ui_text_delete(old);
	fplinux_ui_text_set_change_cb(field, field_changed, NULL);
	fplinux_ui_text_set_focused(field, true);
	caption_field(index);
	reveal_focus();
	sync_commands();
	return;

refused:
	error = errno;
	fplinux_ui_text_delete(field);
	errno = error;
	apidemo_error(edit.app, call);
}

/* An item as a form has one: a muted caption that wraps, then the field. */
static void create_item(size_t index)
{
	lv_obj_t *item = lv_obj_create(edit.column);
	lv_obj_t *caption;

	lv_obj_remove_style_all(item);
	lv_obj_set_size(item, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(item, edit.app->metrics->gap_small, 0);
	lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(item, false);
	caption = lv_label_create(item);
	lv_obj_set_width(caption, lv_pct(100));
	lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(caption, edit.app->theme->font, 0);
	lv_obj_set_style_text_color(caption, edit.app->theme->muted, 0);
	edit.items[index] = item;
	edit.captions[index] = caption;
}

/*
 * A column that scrolls when the fields do not fit, laid out from the
 * metrics as a form is: equal margins and room only when needed for
 * the scroll indicator, spacing between the parts.
 */
static void open_fields(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_metrics *metrics = app->metrics;
	struct fplinux_ui_screen *canvas =
		fplinux_ui_canvas_new(app->ui, _(entry->title));
	lv_obj_t *column;

	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	edit = (struct edit_state){ .app = app, .canvas = canvas };
	column = lv_obj_create(fplinux_ui_canvas_object(canvas));
	lv_obj_remove_style_all(column);
	lv_obj_set_size(column, lv_pct(100), lv_pct(100));
	lv_obj_set_style_pad_left(column, metrics->margin, 0);
	lv_obj_set_style_pad_right(column, metrics->margin, 0);
	lv_obj_set_style_pad_ver(column, metrics->spacing, 0);
	lv_obj_set_style_pad_row(column, metrics->spacing, 0);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
	apidemo_style_scroll(app, column);
	lv_obj_add_event_cb(column, edit_deleted, LV_EVENT_DELETE, NULL);
	edit.column = column;
	create_item(0);
	create_item(1);
	if (!create_field(0) || !create_field(1)) {
		int error = errno;

		/* The canvas cannot be deleted before it is pushed: show it
		 * with Back, under the error. */
		apidemo_add_back(canvas);
		fplinux_ui_screen_set_listener(canvas, apidemo_back_listener,
					       NULL, app);
		fplinux_ui_push(app->ui, canvas);
		errno = error;
		apidemo_error(app, "fplinux_ui_text_create");
		return;
	}
	/* RIGHT_NONE and options false: the first sync adds every command. */
	fplinux_ui_screen_set_listener(canvas, edit_command, NULL, app);
	fplinux_ui_screen_set_key_filter(canvas, edit_filter, NULL);
	fplinux_ui_canvas_set_key_handler(canvas, edit_key, NULL);
	fplinux_ui_canvas_set_key_repeat(canvas,
					 FPLINUX_UI_CANVAS_REPEAT_KEYMAP);
	edit.focus = 0;
	fplinux_ui_text_set_focused(edit.fields[0], true);
	sync_commands();
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
}

static const struct apidemo_entry entries[] = {
	{ N_("Text editor"),
	  "fplinux_ui_text_box_new · fplinux_ui_text_box_get · "
	  "fplinux_ui_text_box_set · max_chars",
	  open_box, 0, N_("Edit several lines with a character limit") },
	{ N_("Read-only box"),
	  "fplinux_ui_text_box_set_constraint · FPLINUX_UI_TEXT_UNEDITABLE",
	  open_viewer, 0, N_("Read and scroll a document") },
	{ N_("Canvas fields"),
	  "fplinux_ui_text_create · fplinux_ui_text_key · "
	  "fplinux_ui_text_set_focused · fplinux_ui_text_delete",
	  open_fields, 0, N_("Handle text input on a Canvas") },
};

const struct apidemo_category apidemo_text = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
};
