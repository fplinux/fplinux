/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-ui-screen-model.h"

#include <stdio.h>
#include <string.h>

/* True when command a comes before command b. */
static bool before(const struct fplinux_ui_command_entry *commands, size_t a,
		   size_t b)
{
	return commands[a].priority < commands[b].priority ||
	       (commands[a].priority == commands[b].priority && a < b);
}

static size_t first_with_role(const struct fplinux_ui_command_entry *commands,
			      size_t count, enum fplinux_ui_command_role role)
{
	size_t found = FPLINUX_UI_COMMAND_NONE;

	for (size_t i = 0; i < count; ++i)
		if (commands[i].role == role &&
		    (found == FPLINUX_UI_COMMAND_NONE ||
		     before(commands, i, found)))
			found = i;
	return found;
}

void fplinux_ui_command_keys_assign(
	const struct fplinux_ui_command_entry *commands, size_t count,
	bool element_acts, bool right_taken,
	struct fplinux_ui_command_keys *keys, size_t *menu)
{
	size_t right = first_with_role(commands, count, FPLINUX_UI_ROLE_BACK);
	size_t ok = element_acts ? FPLINUX_UI_COMMAND_NONE :
				   first_with_role(commands, count,
						   FPLINUX_UI_ROLE_OK);
	bool help = false;
	size_t remaining = 0;

	*keys = (struct fplinux_ui_command_keys){
		.right = right_taken ? FPLINUX_UI_COMMAND_NONE : right,
		.ok = ok,
		.left = FPLINUX_UI_COMMAND_NONE,
	};
	for (size_t i = 0; i < count; ++i) {
		size_t at;

		if ((!right_taken && i == right) || i == ok)
			continue;
		help = help || commands[i].role == FPLINUX_UI_ROLE_HELP;
		at = remaining++;
		/* Insertion in priority order; the lists are short. */
		while (at && before(commands, i, menu[at - 1])) {
			menu[at] = menu[at - 1];
			--at;
		}
		menu[at] = i;
	}
	/* Clear displaces Back into Options, never out of the command set. */
	if (remaining == 1 && !help &&
	    !(right_taken && right != FPLINUX_UI_COMMAND_NONE)) {
		keys->left = menu[0];
	} else if (remaining) {
		keys->options = true;
		keys->menu_count = remaining;
	}
}

void fplinux_ui_command_keys_collapse(struct fplinux_ui_command_keys *keys,
				      size_t *menu)
{
	if (keys->left == FPLINUX_UI_COMMAND_NONE)
		return;
	memmove(menu + 1, menu, keys->menu_count * sizeof(*menu));
	menu[0] = keys->left;
	keys->left = FPLINUX_UI_COMMAND_NONE;
	keys->options = true;
	++keys->menu_count;
}

size_t fplinux_ui_index_inserted(size_t index, size_t at, size_t count)
{
	return index != SIZE_MAX && index >= at ? index + count : index;
}

size_t fplinux_ui_index_deleted(size_t index, size_t at, size_t count)
{
	if (index == SIZE_MAX || index < at)
		return index;
	return index - at < count ? SIZE_MAX : index - count;
}

size_t fplinux_ui_focus_deleted(size_t focus, size_t at, size_t count,
				size_t size)
{
	size_t index;

	if (at >= size)
		return focus;
	if (count > size - at)
		count = size - at;
	if (count == size)
		return SIZE_MAX;
	index = fplinux_ui_index_deleted(focus, at, count);
	if (index != SIZE_MAX || focus == SIZE_MAX)
		return index;
	return at < size - count ? at : size - count - 1;
}

size_t fplinux_ui_focus_next(size_t from, size_t count,
			     bool (*focusable)(size_t index, void *context),
			     void *context)
{
	for (size_t i = from; i < count; ++i)
		if (focusable(i, context))
			return i;
	for (size_t i = from < count ? from : count; i--;)
		if (focusable(i, context))
			return i;
	return SIZE_MAX;
}

size_t fplinux_ui_focus_step(size_t focus, size_t count, long rows)
{
	if (!count)
		return SIZE_MAX;
	if (focus >= count)
		return 0;
	if (rows < 0) {
		/* Negated as unsigned, which LONG_MIN survives. */
		unsigned long back = -(unsigned long)rows;

		return back >= focus ? 0 : focus - (size_t)back;
	}
	if ((unsigned long)rows >= count - 1 - focus)
		return count - 1;
	return focus + (size_t)rows;
}

/* The largest scroll offset. */
static int64_t top_max(const struct fplinux_ui_virtual_window *window)
{
	int64_t over = (int64_t)window->count * window->pitch - window->view;

	return over > 0 ? over : 0;
}

/*
 * Scrolls to top, kept within the content, then the least that shows the
 * focused row wholly.
 */
static void show_focus(struct fplinux_ui_virtual_window *window, int64_t top)
{
	int64_t high = top_max(window);

	top = top < 0 ? 0 : top > high ? high : top;
	if (window->focus != SIZE_MAX) {
		int64_t row = (int64_t)window->focus * window->pitch;

		if (row < top)
			top = row;
		else if (row + window->pitch > top + window->view)
			top = row + window->pitch - window->view;
	}
	window->top = (int32_t)top;
}

/* The focused row's place in the view. */
static int64_t focus_offset(const struct fplinux_ui_virtual_window *window)
{
	if (window->focus == SIZE_MAX)
		return 0;
	return (int64_t)window->focus * window->pitch - window->top;
}

/* Puts the focused row at offset in the view when the content allows. */
static void keep_focus(struct fplinux_ui_virtual_window *window, int64_t offset)
{
	if (window->focus == SIZE_MAX)
		show_focus(window, 0);
	else
		show_focus(window,
			   (int64_t)window->focus * window->pitch - offset);
}

/* The most rows the view shows in part, with one cut at each end. */
static size_t pool_size(const struct fplinux_ui_virtual_window *window)
{
	size_t slots = 1;

	if (window->view > 0)
		slots += (size_t)(((int64_t)window->view + window->pitch - 2) /
				  window->pitch);
	if (slots > FPLINUX_UI_VIRTUAL_SLOTS_MAX)
		slots = FPLINUX_UI_VIRTUAL_SLOTS_MAX;
	return slots < window->count ? slots : window->count;
}

/*
 * Fits the pool to the view and lists the slots whose row changed or is one
 * of the changed rows dirty .. dirty + changed - 1.
 */
static size_t bind(struct fplinux_ui_virtual_window *window, size_t dirty,
		   size_t changed, size_t *rebind)
{
	size_t slots = pool_size(window);
	size_t count = 0;

	window->slots = slots;
	window->first = (size_t)(window->top / window->pitch);
	if (window->first > window->count - slots)
		window->first = window->count - slots;
	for (size_t slot = 0; slot < FPLINUX_UI_VIRTUAL_SLOTS_MAX; ++slot) {
		size_t row = SIZE_MAX;

		if (slot < slots)
			row = window->first +
			      (slot + slots - window->first % slots) % slots;
		if (row != window->bound[slot] ||
		    (row != SIZE_MAX && row >= dirty && row - dirty < changed))
			rebind[count++] = slot;
		window->bound[slot] = row;
	}
	return count;
}

size_t fplinux_ui_virtual_init(struct fplinux_ui_virtual_window *window,
			       size_t count, int32_t pitch, int32_t view,
			       size_t *rebind)
{
	*window = (struct fplinux_ui_virtual_window){
		.count = count,
		.pitch = pitch > 0 ? pitch : 1,
		.view = view > 0 ? view : 0,
		.focus = count ? 0 : SIZE_MAX,
	};
	for (size_t slot = 0; slot < FPLINUX_UI_VIRTUAL_SLOTS_MAX; ++slot)
		window->bound[slot] = SIZE_MAX;
	return bind(window, 0, 0, rebind);
}

size_t fplinux_ui_virtual_set_focus(struct fplinux_ui_virtual_window *window,
				    size_t focus, size_t *rebind)
{
	if (focus >= window->count)
		return 0;
	window->focus = focus;
	show_focus(window, window->top);
	return bind(window, 0, 0, rebind);
}

size_t fplinux_ui_virtual_inserted(struct fplinux_ui_virtual_window *window,
				   size_t at, size_t count, size_t *rebind)
{
	int64_t offset = focus_offset(window);

	if (!count || at > window->count)
		return 0;
	window->count += count;
	window->focus =
		window->focus == SIZE_MAX ?
			0 :
			fplinux_ui_index_inserted(window->focus, at, count);
	keep_focus(window, offset);
	return bind(window, at, SIZE_MAX, rebind);
}

size_t fplinux_ui_virtual_deleted(struct fplinux_ui_virtual_window *window,
				  size_t at, size_t count, size_t *rebind)
{
	int64_t offset = focus_offset(window);

	if (!count || at >= window->count)
		return 0;
	if (count > window->count - at)
		count = window->count - at;
	window->focus = fplinux_ui_focus_deleted(window->focus, at, count,
						 window->count);
	window->count -= count;
	keep_focus(window, offset);
	return bind(window, at, SIZE_MAX, rebind);
}

size_t fplinux_ui_virtual_invalidate(struct fplinux_ui_virtual_window *window,
				     size_t first, size_t count, size_t *rebind)
{
	return bind(window, first, count, rebind);
}

size_t fplinux_ui_virtual_resize(struct fplinux_ui_virtual_window *window,
				 int32_t pitch, int32_t view, size_t *rebind)
{
	int64_t offset = focus_offset(window);
	bool moved;

	pitch = pitch > 0 ? pitch : 1;
	moved = pitch != window->pitch;
	window->pitch = pitch;
	window->view = view > 0 ? view : 0;
	keep_focus(window, offset);
	/* A new pitch moves every row. */
	return bind(window, 0, moved ? SIZE_MAX : 0, rebind);
}

int32_t fplinux_ui_reveal_scroll(int32_t view_top, int32_t view_bottom,
				 int32_t top, int32_t bottom)
{
	if (top < view_top || bottom - top > view_bottom - view_top)
		return view_top - top;
	if (bottom > view_bottom)
		return view_bottom - bottom;
	return 0;
}

/* Translations are in the runtime's fplinux-ui catalogues. */
static const char *const labels[FPLINUX_UI_LABEL_COUNT] = {
	[FPLINUX_UI_LABEL_OPTIONS] = "Options",
	[FPLINUX_UI_LABEL_SELECT] = "Select",
	[FPLINUX_UI_LABEL_MARK] = "Mark",
	[FPLINUX_UI_LABEL_UNMARK] = "Unmark",
	[FPLINUX_UI_LABEL_EDIT] = "Edit",
	[FPLINUX_UI_LABEL_DONE] = "Done",
	[FPLINUX_UI_LABEL_CLOSE] = "Close",
	[FPLINUX_UI_LABEL_CLEAR] = "Clear",
	[FPLINUX_UI_LABEL_BACK] = "Back",
	[FPLINUX_UI_LABEL_CANCEL] = "Cancel",
	[FPLINUX_UI_LABEL_EXIT] = "Exit",
	[FPLINUX_UI_LABEL_STOP] = "Stop",
	[FPLINUX_UI_LABEL_OK] = "OK",
	[FPLINUX_UI_LABEL_HELP] = "Help",
	[FPLINUX_UI_LABEL_NEW_LINE] = "New line",
	[FPLINUX_UI_LABEL_INSERT] = "Insert",
	[FPLINUX_UI_LABEL_INFORMATION] = "Information",
	[FPLINUX_UI_LABEL_CONFIRMATION] = "Confirmation",
	[FPLINUX_UI_LABEL_WARNING] = "Warning",
	[FPLINUX_UI_LABEL_ERROR] = "Error",
};

const char *fplinux_ui_label(enum fplinux_ui_label label)
{
	if ((unsigned int)label >= FPLINUX_UI_LABEL_COUNT)
		return "";
	return labels[label];
}

static const int limits[FPLINUX_UI_DATE_PART_COUNT][2] = {
	[FPLINUX_UI_DATE_DAY] = { 1, 31 },
	[FPLINUX_UI_DATE_MONTH] = { 1, 12 },
	[FPLINUX_UI_DATE_YEAR] = { FPLINUX_UI_DATE_YEAR_MIN,
				   FPLINUX_UI_DATE_YEAR_MAX },
	[FPLINUX_UI_DATE_HOUR] = { 0, 23 },
	[FPLINUX_UI_DATE_MINUTE] = { 0, 59 },
};

static int clamp(int value, int low, int high)
{
	return value < low ? low : value > high ? high : value;
}

static int days_in_month(int year, int month)
{
	static const int days[] = { 31, 28, 31, 30, 31, 30,
				    31, 31, 30, 31, 30, 31 };
	bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

	if (month < 1 || month > 12)
		return 31;
	return month == 2 && leap ? 29 : days[month - 1];
}

static int part_high(const struct fplinux_ui_date_edit *edit,
		     enum fplinux_ui_date_part part)
{
	if (part == FPLINUX_UI_DATE_DAY)
		return days_in_month(edit->values[FPLINUX_UI_DATE_YEAR],
				     edit->values[FPLINUX_UI_DATE_MONTH]);
	return limits[part][1];
}

static unsigned int part_width(enum fplinux_ui_date_part part)
{
	return part == FPLINUX_UI_DATE_YEAR ? 4 : 2;
}

/*
 * The day chosen, shortened by a shorter month or a non-leap year; the
 * choice stays, so a longer month later gives it back.
 */
static void fit_day(struct fplinux_ui_date_edit *edit)
{
	edit->values[FPLINUX_UI_DATE_DAY] =
		clamp(edit->day, 1, part_high(edit, FPLINUX_UI_DATE_DAY));
}

/* The date part of a placeholder, or FPLINUX_UI_DATE_PART_COUNT. */
static enum fplinux_ui_date_part placeholder_part(const char *name,
						  size_t length)
{
	static const struct {
		const char *name;
		enum fplinux_ui_date_part part;
	} names[] = {
		{ "dd", FPLINUX_UI_DATE_DAY },
		{ "day", FPLINUX_UI_DATE_DAY },
		{ "mm", FPLINUX_UI_DATE_MONTH },
		{ "month", FPLINUX_UI_DATE_MONTH },
		{ "month.genitive", FPLINUX_UI_DATE_MONTH },
		{ "yyyy", FPLINUX_UI_DATE_YEAR },
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		if (strlen(names[i].name) == length &&
		    !strncmp(name, names[i].name, length))
			return names[i].part;
	return FPLINUX_UI_DATE_PART_COUNT;
}

/* Appends part with the text before it; false when it does not fit. */
static bool add_part(struct fplinux_ui_date_edit *edit,
		     enum fplinux_ui_date_part part, const char *separator,
		     size_t length)
{
	if (length >= FPLINUX_UI_DATE_SEPARATOR_BYTES)
		return false;
	memcpy(edit->separator[edit->count], separator, length);
	edit->separator[edit->count][length] = '\0';
	edit->order[edit->count++] = part;
	return true;
}

/*
 * Lays out day, month and year as pattern shows them; false if it cannot.
 * Text before the first part and after the last is left out.
 */
static bool add_date_parts(struct fplinux_ui_date_edit *edit,
			   const char *pattern)
{
	unsigned int seen = 0;

	while (pattern && *pattern) {
		size_t literal = strcspn(pattern, "{");
		const char *name = pattern + literal + 1;
		const char *close;
		enum fplinux_ui_date_part part;

		if (!pattern[literal])
			break;
		close = strchr(name, '}');
		if (!close)
			return false;
		part = placeholder_part(name, (size_t)(close - name));
		if (part == FPLINUX_UI_DATE_PART_COUNT ||
		    (seen & (1U << part)) ||
		    !add_part(edit, part, pattern, seen ? literal : 0))
			return false;
		seen |= 1U << part;
		pattern = close + 1;
	}
	return seen ==
	       (1U << FPLINUX_UI_DATE_DAY | 1U << FPLINUX_UI_DATE_MONTH |
		1U << FPLINUX_UI_DATE_YEAR);
}

void fplinux_ui_date_edit_begin(struct fplinux_ui_date_edit *edit, time_t value,
				bool date, bool time, const char *pattern)
{
	int *values = edit->values;

	memset(edit, 0, sizeof(*edit));
	if (!localtime_r(&value, &edit->time)) {
		edit->time.tm_year = 2000 - 1900;
		edit->time.tm_mday = 1;
	}
	edit->day = clamp(edit->time.tm_mday, 1, 31);
	values[FPLINUX_UI_DATE_MONTH] = clamp(edit->time.tm_mon + 1, 1, 12);
	values[FPLINUX_UI_DATE_YEAR] = clamp(edit->time.tm_year + 1900,
					     FPLINUX_UI_DATE_YEAR_MIN,
					     FPLINUX_UI_DATE_YEAR_MAX);
	values[FPLINUX_UI_DATE_HOUR] = clamp(edit->time.tm_hour, 0, 23);
	values[FPLINUX_UI_DATE_MINUTE] = clamp(edit->time.tm_min, 0, 59);
	fit_day(edit);
	if (date || !time) {
		if (!add_date_parts(edit, pattern)) {
			edit->count = 0;
			add_date_parts(edit, FPLINUX_UI_DATE_DEFAULT_PATTERN);
		}
	}
	if (time) {
		add_part(edit, FPLINUX_UI_DATE_HOUR, " ", edit->count ? 1 : 0);
		add_part(edit, FPLINUX_UI_DATE_MINUTE, ":", 1);
	}
	edit->part = edit->order[0];
}

static void complete_part(struct fplinux_ui_date_edit *edit)
{
	int *value = &edit->values[edit->part];

	*value = clamp(*value, limits[edit->part][0], limits[edit->part][1]);
	if (edit->part == FPLINUX_UI_DATE_DAY)
		edit->day = *value;
	edit->typed = 0;
	fit_day(edit);
}

void fplinux_ui_date_edit_finish(struct fplinux_ui_date_edit *edit)
{
	if (edit->typed)
		complete_part(edit);
	fit_day(edit);
}

void fplinux_ui_date_edit_step(struct fplinux_ui_date_edit *edit, int delta)
{
	enum fplinux_ui_date_part part = edit->part;
	int low = limits[part][0];
	int span;

	fplinux_ui_date_edit_finish(edit);
	span = part_high(edit, part) - low + 1;
	edit->values[part] =
		low + ((edit->values[part] - low + delta) % span + span) % span;
	if (part == FPLINUX_UI_DATE_DAY)
		edit->day = edit->values[part];
	fit_day(edit);
}

void fplinux_ui_date_edit_move(struct fplinux_ui_date_edit *edit, int delta)
{
	int count = (int)edit->count;

	fplinux_ui_date_edit_finish(edit);
	edit->position =
		(unsigned int)((((int)edit->position + delta) % count + count) %
			       count);
	edit->part = edit->order[edit->position];
}

void fplinux_ui_date_edit_digit(struct fplinux_ui_date_edit *edit,
				unsigned int digit)
{
	int *value = &edit->values[edit->part];

	if (digit > 9)
		return;
	*value = edit->typed ? *value * 10 + (int)digit : (int)digit;
	if (++edit->typed < part_width(edit->part))
		return;
	complete_part(edit);
	if (edit->position + 1 < edit->count)
		edit->part = edit->order[++edit->position];
}

time_t fplinux_ui_date_edit_value(const struct fplinux_ui_date_edit *edit)
{
	struct fplinux_ui_date_edit copy = *edit;
	struct tm time = edit->time;

	fplinux_ui_date_edit_finish(&copy);
	time.tm_mday = copy.values[FPLINUX_UI_DATE_DAY];
	time.tm_mon = copy.values[FPLINUX_UI_DATE_MONTH] - 1;
	time.tm_year = copy.values[FPLINUX_UI_DATE_YEAR] - 1900;
	time.tm_hour = copy.values[FPLINUX_UI_DATE_HOUR];
	time.tm_min = copy.values[FPLINUX_UI_DATE_MINUTE];
	time.tm_isdst = -1;
	return mktime(&time);
}

/* UTF-8 characters in text: the bytes that do not continue one. */
static size_t count_characters(const char *text)
{
	size_t count = 0;

	for (; *text; ++text)
		if (((unsigned char)*text & 0xc0) != 0x80)
			++count;
	return count;
}

void fplinux_ui_date_edit_format(const struct fplinux_ui_date_edit *edit,
				 char *text, size_t size, size_t *start,
				 size_t *end)
{
	size_t length = 0;
	size_t characters = 0;

	*start = *end = 0;
	if (size)
		text[0] = '\0';
	for (unsigned int i = 0; i < edit->count; ++i) {
		enum fplinux_ui_date_part part = edit->order[i];
		int written = snprintf(text + length, size - length, "%s",
				       edit->separator[i]);

		if (written < 0 || (size_t)written >= size - length)
			return;
		length += (size_t)written;
		characters += count_characters(edit->separator[i]);
		if (i == edit->position)
			*start = characters;
		written = snprintf(text + length, size - length, "%0*d",
				   (int)part_width(part), edit->values[part]);
		if (written < 0 || (size_t)written >= size - length)
			return;
		length += (size_t)written;
		/* Digits take a byte each. */
		characters += (size_t)written;
		if (i == edit->position)
			*end = characters;
	}
}
