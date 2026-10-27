/* SPDX-License-Identifier: GPL-2.0-only */
#include "launcher-layout.h"

static int at_least(int value, int minimum)
{
	return value > minimum ? value : minimum;
}

void fplinux_launcher_frame(const struct fplinux_ui_profile_spec *spec,
			    int width, int height,
			    struct fplinux_launcher_frame *frame)
{
	frame->width = width;
	frame->height = height;
	frame->status_height = spec->status_height;
	frame->softkey_height = spec->softkey_height;
	frame->body_y = spec->status_height;
	frame->body_height = at_least(
		height - spec->status_height - spec->softkey_height, 0);
	frame->band_height = spec->header_height;
	frame->viewport_height =
		at_least(frame->body_height - spec->header_height, 0);
}

int fplinux_launcher_centre(int extent, int size)
{
	return (extent - size) / 2;
}

void fplinux_launcher_launch_layout(const struct fplinux_ui_profile_spec *spec,
				    const struct fplinux_launcher_frame *frame,
				    struct fplinux_launcher_launch *layout)
{
	int spinner = spec->spinner_size;
	int middle = frame->body_height / 2;
	int gap = spec->alert_gap;
	int icon = spec->app_icon_size;
	int text_y = middle + spinner / 2 + gap;

	layout->spinner = (struct fplinux_launcher_rect){
		.x = fplinux_launcher_centre(frame->width, spinner),
		.y = middle - spinner / 2,
		.width = spinner,
		.height = spinner,
	};
	layout->icon = (struct fplinux_launcher_rect){
		.x = fplinux_launcher_centre(frame->width, icon),
		.y = layout->spinner.y - gap - icon,
		.width = icon,
		.height = icon,
	};
	layout->text = (struct fplinux_launcher_rect){
		.x = spec->margin,
		.y = text_y,
		.width = fplinux_launcher_text_width(spec, frame->width),
		.height = at_least(frame->body_height - text_y - gap, 0),
	};
}

void fplinux_launcher_battery_layout(const struct fplinux_ui_profile_spec *spec,
				     int width,
				     struct fplinux_launcher_battery *battery)
{
	int body_width = spec->battery_width;
	int body_height = spec->battery_height;
	int bar_width = spec->battery_bar_width;
	int bar_height = spec->battery_bar_height;
	int steps = FPLINUX_LAUNCHER_BATTERY_STEP_COUNT;
	/* Bars 1 px apart inside the 1 px outline, centred when room is left. */
	int outline = 1;
	int inner = body_width - 2 * outline;
	int pad = at_least((inner - (steps * bar_width + steps - 1)) / 2, 0);
	int dot = at_least(bar_height / 4, 1);
	int bolt = spec->bolt_height;
	/* The bolt is as wide as half its height; its stroke about a fifth. */
	int middle = (bolt + 1) / 2;
	struct fplinux_launcher_rect *body = &battery->body;

	battery->cap.width = spec->battery_cap_width;
	battery->cap.height = spec->battery_cap_height;
	battery->cap.x = width - spec->margin - battery->cap.width;
	body->width = body_width;
	body->height = body_height;
	body->x = battery->cap.x - body_width;
	body->y = fplinux_launcher_centre(spec->status_height, body_height);
	battery->cap.y = body->y + fplinux_launcher_centre(body_height,
							   battery->cap.height);
	battery->outline_width = outline;
	battery->outline_radius = outline;
	for (int i = 0; i < steps; ++i)
		battery->bars[i] = (struct fplinux_launcher_rect){
			.x = body->x + outline + pad + i * (bar_width + 1),
			.y = body->y +
			     fplinux_launcher_centre(body_height, bar_height),
			.width = bar_width,
			.height = bar_height,
		};
	battery->low_stem = (struct fplinux_launcher_rect){
		.x = body->x + fplinux_launcher_centre(body_width, bar_width),
		.y = battery->bars[0].y,
		.width = bar_width,
		.height = bar_height - dot - 1,
	};
	battery->low_dot = (struct fplinux_launcher_rect){
		.x = battery->low_stem.x,
		.y = battery->low_stem.y + battery->low_stem.height + 1,
		.width = bar_width,
		.height = dot,
	};
	battery->bolt = (struct fplinux_launcher_rect){
		.x = body->x - spec->gap_small - middle,
		.y = fplinux_launcher_centre(spec->status_height, bolt),
		.width = middle,
		.height = bolt,
	};
	battery->bolt_points[0][0] = middle;
	battery->bolt_points[0][1] = 1;
	battery->bolt_points[1][0] = 1;
	battery->bolt_points[1][1] = middle;
	battery->bolt_points[2][0] = middle;
	battery->bolt_points[2][1] = middle;
	battery->bolt_points[3][0] = 1;
	battery->bolt_points[3][1] = bolt;
	battery->bolt_line_width = at_least(bolt / 5, 1);
}

struct fplinux_launcher_rect
fplinux_launcher_status_glyph(const struct fplinux_ui_profile_spec *spec)
{
	int size = spec->status_icon_size;

	return (struct fplinux_launcher_rect){
		.x = spec->margin,
		.y = fplinux_launcher_centre(spec->status_height, size),
		.width = size,
		.height = size,
	};
}

size_t fplinux_launcher_columns(const struct fplinux_ui_profile_spec *spec)
{
	return spec->grid_columns > 0 ? (size_t)spec->grid_columns : 1;
}

struct fplinux_launcher_rect
fplinux_launcher_viewport(const struct fplinux_launcher_frame *frame)
{
	return (struct fplinux_launcher_rect){
		.y = frame->band_height,
		.width = frame->width,
		.height = frame->viewport_height,
	};
}

int fplinux_launcher_content_height(
	const struct fplinux_launcher_scroll *scroll, size_t rows)
{
	if (!rows)
		return 0;
	return 2 * scroll->padding + ((int)rows - 1) * scroll->pitch +
	       scroll->item;
}

int fplinux_launcher_scroll_offset(const struct fplinux_launcher_scroll *scroll,
				   size_t rows, int viewport_height, int offset,
				   size_t row)
{
	int last =
		fplinux_launcher_content_height(scroll, rows) - viewport_height;
	int top = scroll->padding + (int)row * scroll->pitch;
	int bottom = top + scroll->item + scroll->reveal_margin;

	if (!rows || !row)
		return 0;

	if (bottom > offset + viewport_height)
		offset = bottom - viewport_height;
	/* A viewport lower than the row shows the row's top. */
	if (top - scroll->reveal_margin < offset)
		offset = top - scroll->reveal_margin;
	if (offset > last)
		offset = last;
	return at_least(offset, 0);
}

size_t
fplinux_launcher_first_visible_row(const struct fplinux_launcher_scroll *scroll,
				   size_t rows, int offset)
{
	size_t row = 0;

	if (offset > scroll->padding)
		row = (size_t)((offset - scroll->padding + scroll->pitch - 1) /
			       scroll->pitch);
	return rows && row >= rows ? rows - 1 : row;
}

size_t fplinux_launcher_move(size_t selected, size_t count, size_t columns,
			     enum fplinux_launcher_move move)
{
	size_t last;

	if (!count)
		return 0;
	last = count - 1;
	if (selected > last)
		selected = last;
	if (!columns)
		columns = 1;
	switch (move) {
	case FPLINUX_LAUNCHER_MOVE_UP:
		if (selected >= columns)
			return selected - columns;
		return selected < last % columns ?
			       last - last % columns + selected :
			       last;
	case FPLINUX_LAUNCHER_MOVE_DOWN:
		if (selected / columns == last / columns)
			return selected % columns;
		return selected + columns < last ? selected + columns : last;
	case FPLINUX_LAUNCHER_MOVE_LEFT:
		if (!selected)
			return last;
		return selected - 1;
	case FPLINUX_LAUNCHER_MOVE_RIGHT:
		if (selected == last)
			return 0;
		return selected + 1;
	}
	return selected;
}

size_t fplinux_launcher_digit_index(size_t first_row, size_t columns, int digit,
				    size_t count)
{
	size_t index;

	if (digit < 1 || digit > 9)
		return count;
	index = first_row * columns + (size_t)digit - 1;
	return index < count ? index : count;
}

static struct fplinux_launcher_shape around(struct fplinux_launcher_shape shape,
					    int distance)
{
	return (struct fplinux_launcher_shape){
		.box = {
			.x = shape.box.x - distance,
			.y = shape.box.y - distance,
			.width = shape.box.width + 2 * distance,
			.height = shape.box.height + 2 * distance,
		},
		.radius = shape.radius + distance,
	};
}

int fplinux_launcher_grid_x(int width, int columns, int size, int column)
{
	int spare = width - columns * size;
	/* spare / (columns + 1) to the nearest pixel, a half down. */
	int edge = (2 * spare + columns) / (2 * (columns + 1));

	if (columns < 2)
		return fplinux_launcher_centre(width, size);
	return edge + column * size +
	       column * (spare - 2 * edge) / (columns - 1);
}

int fplinux_launcher_grid_gap(int width, int columns, int size)
{
	int gap = columns < 2 ?
			  fplinux_launcher_grid_x(width, columns, size, 0) :
			  fplinux_launcher_grid_x(width, columns, size, 1) -
				  fplinux_launcher_grid_x(width, columns, size,
							  0) -
				  size;

	return at_least(gap, 0);
}

void fplinux_launcher_grid_cell(const struct fplinux_ui_profile_spec *spec,
				int width,
				const struct fplinux_launcher_scroll *scroll,
				size_t column, size_t row,
				struct fplinux_launcher_cell *cell)
{
	int columns = (int)fplinux_launcher_columns(spec);
	int size = spec->app_icon_size;
	int inset = spec->grid_tile_inset;

	cell->tile = (struct fplinux_launcher_shape){
		.box = {
			.x = fplinux_launcher_grid_x(width, columns, size,
						     (int)column),
			.y = scroll->padding + (int)row * scroll->pitch,
			.width = size,
			.height = size,
		},
		.radius = spec->app_tile_radius,
	};
	cell->ring = around(cell->tile, spec->grid_ring_gap +
						spec->grid_ring_width - inset);
	cell->inset = inset;
}

void fplinux_launcher_scroll_layout(const struct fplinux_ui_profile_spec *spec,
				    int width, int viewport_height,
				    struct fplinux_launcher_scroll *scroll)
{
	int size = spec->app_icon_size;
	int gap;
	int padding;

	gap = fplinux_launcher_grid_gap(
		width, (int)fplinux_launcher_columns(spec), size);
	padding = gap;
	/* A clipped next row must be a useful hint, not a thin coloured strip. */
	if (viewport_height > gap) {
		int rows = (viewport_height - gap) / (size + gap);
		int visible = viewport_height - gap - rows * (size + gap);

		if (rows > 0 && visible > 0 && 3 * visible < size) {
			int adjusted = gap + (visible + rows) / (rows + 1);
			int fitting =
				(viewport_height - rows * size) / (rows + 1);

			gap = adjusted < fitting ? adjusted : fitting;
			padding = (viewport_height - rows * size -
				   (rows - 1) * gap) /
				  2;
		}
	}
	scroll->padding = padding;
	scroll->pitch = size + gap;
	scroll->item = size;
	scroll->reveal_margin = spec->grid_ring_gap + spec->grid_ring_width -
				spec->grid_tile_inset + spec->gap_small;
}

void fplinux_launcher_home_layout(const struct fplinux_ui_profile_spec *spec,
				  int body_height, int clock_height,
				  int date_height, int hint_height,
				  struct fplinux_launcher_home *home)
{
	int heights[] = { clock_height, date_height, hint_height };
	int *tops[] = { &home->clock_y, &home->date_y, &home->hint_y };
	int total = 0;
	int y;

	for (int i = 0; i < 3; ++i)
		if (heights[i] > 0)
			total += (total ? spec->home_gap : 0) + heights[i];
	y = at_least(fplinux_launcher_centre(body_height, total), 0);
	for (int i = 0; i < 3; ++i) {
		*tops[i] = y;
		if (heights[i] > 0)
			y += heights[i] + spec->home_gap;
	}
}

int fplinux_launcher_text_width(const struct fplinux_ui_profile_spec *spec,
				int width)
{
	return at_least(width - 2 * spec->margin, 0);
}

void fplinux_launcher_notice_layout(const struct fplinux_ui_profile_spec *spec,
				    int width, int body_height,
				    struct fplinux_launcher_notice_box *box)
{
	box->width = width - 2 * spec->alert_margin;
	box->max_height = body_height - 2 * spec->alert_margin;
	box->border = spec->alert_border;
	box->padding = spec->alert_padding;
	box->gap = spec->alert_gap;
}

size_t fplinux_launcher_icon_sizes(int sizes[], size_t capacity)
{
	/* Keep the documented 48-pixel source size for existing packages. */
	int found[2 * FPLINUX_UI_PROFILE_COUNT + 1] = { 48 };
	size_t count = 1;

	for (int profile = 0; profile < FPLINUX_UI_PROFILE_COUNT; ++profile) {
		const struct fplinux_ui_profile_spec *spec =
			fplinux_ui_profile_spec(profile);
		int pair[] = { spec->row_icon_size, spec->app_icon_size };

		/* Insertion into the ascending list, skipping repeats. */
		for (int i = 0; i < 2; ++i) {
			size_t position = 0;

			while (position < count && found[position] < pair[i])
				++position;
			if (position < count && found[position] == pair[i])
				continue;
			for (size_t j = count; j > position; --j)
				found[j] = found[j - 1];
			found[position] = pair[i];
			++count;
		}
	}
	for (size_t i = 0; i < count && i < capacity; ++i)
		sizes[i] = found[i];
	return count;
}
