// SPDX-License-Identifier: GPL-2.0-only
#include "boot-screen.h"

#define COLOUR_BACKGROUND 0x0841u
#define COLOUR_SURFACE 0x18c3u
#define COLOUR_PRIMARY 0xf79eu
#define COLOUR_MUTED 0xbdf7u
#define COLOUR_RULE 0x4208u
#define COLOUR_DIM 0x6b4du
#define COLOUR_ERROR 0xe30cu

#define FULL_WIDTH 240u
#define FULL_HEIGHT 320u
#define COMPACT_MIN_WIDTH 128u
#define COMPACT_MIN_HEIGHT 160u

struct glyph {
	char character;
	uint8_t columns[5];
};

/* Five columns per glyph, least significant bit at the top. */
static const struct glyph font[] = {
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ '!', { 0x00, 0x00, 0x5f, 0x00, 0x00 } },
	{ '"', { 0x00, 0x07, 0x00, 0x07, 0x00 } },
	{ '#', { 0x14, 0x7f, 0x14, 0x7f, 0x14 } },
	{ '$', { 0x24, 0x2a, 0x7f, 0x2a, 0x12 } },
	{ '%', { 0x23, 0x13, 0x08, 0x64, 0x62 } },
	{ '&', { 0x36, 0x49, 0x55, 0x22, 0x50 } },
	{ '\'', { 0x00, 0x05, 0x03, 0x00, 0x00 } },
	{ '(', { 0x00, 0x1c, 0x22, 0x41, 0x00 } },
	{ ')', { 0x00, 0x41, 0x22, 0x1c, 0x00 } },
	{ '*', { 0x14, 0x08, 0x3e, 0x08, 0x14 } },
	{ '+', { 0x08, 0x08, 0x3e, 0x08, 0x08 } },
	{ ',', { 0x00, 0x50, 0x30, 0x00, 0x00 } },
	{ '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
	{ '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
	{ '/', { 0x20, 0x10, 0x08, 0x04, 0x02 } },
	{ '0', { 0x3e, 0x51, 0x49, 0x45, 0x3e } },
	{ '1', { 0x00, 0x42, 0x7f, 0x40, 0x00 } },
	{ '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
	{ '3', { 0x21, 0x41, 0x45, 0x4b, 0x31 } },
	{ '4', { 0x18, 0x14, 0x12, 0x7f, 0x10 } },
	{ '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
	{ '6', { 0x3c, 0x4a, 0x49, 0x49, 0x30 } },
	{ '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
	{ '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
	{ '9', { 0x06, 0x49, 0x49, 0x29, 0x1e } },
	{ ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } },
	{ ';', { 0x00, 0x56, 0x36, 0x00, 0x00 } },
	{ '<', { 0x08, 0x14, 0x22, 0x41, 0x00 } },
	{ '=', { 0x14, 0x14, 0x14, 0x14, 0x14 } },
	{ '>', { 0x00, 0x41, 0x22, 0x14, 0x08 } },
	{ '?', { 0x02, 0x01, 0x51, 0x09, 0x06 } },
	{ '@', { 0x32, 0x49, 0x79, 0x41, 0x3e } },
	{ 'A', { 0x7e, 0x11, 0x11, 0x11, 0x7e } },
	{ 'B', { 0x7f, 0x49, 0x49, 0x49, 0x36 } },
	{ 'C', { 0x3e, 0x41, 0x41, 0x41, 0x22 } },
	{ 'D', { 0x7f, 0x41, 0x41, 0x22, 0x1c } },
	{ 'E', { 0x7f, 0x49, 0x49, 0x49, 0x41 } },
	{ 'F', { 0x7f, 0x09, 0x09, 0x09, 0x01 } },
	{ 'G', { 0x3e, 0x41, 0x49, 0x49, 0x7a } },
	{ 'H', { 0x7f, 0x08, 0x08, 0x08, 0x7f } },
	{ 'I', { 0x00, 0x41, 0x7f, 0x41, 0x00 } },
	{ 'J', { 0x20, 0x40, 0x41, 0x3f, 0x01 } },
	{ 'K', { 0x7f, 0x08, 0x14, 0x22, 0x41 } },
	{ 'L', { 0x7f, 0x40, 0x40, 0x40, 0x40 } },
	{ 'M', { 0x7f, 0x02, 0x0c, 0x02, 0x7f } },
	{ 'N', { 0x7f, 0x04, 0x08, 0x10, 0x7f } },
	{ 'O', { 0x3e, 0x41, 0x41, 0x41, 0x3e } },
	{ 'P', { 0x7f, 0x09, 0x09, 0x09, 0x06 } },
	{ 'Q', { 0x3e, 0x41, 0x51, 0x21, 0x5e } },
	{ 'R', { 0x7f, 0x09, 0x19, 0x29, 0x46 } },
	{ 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
	{ 'T', { 0x01, 0x01, 0x7f, 0x01, 0x01 } },
	{ 'U', { 0x3f, 0x40, 0x40, 0x40, 0x3f } },
	{ 'V', { 0x1f, 0x20, 0x40, 0x20, 0x1f } },
	{ 'W', { 0x3f, 0x40, 0x38, 0x40, 0x3f } },
	{ 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } },
	{ 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
	{ 'Z', { 0x61, 0x51, 0x49, 0x45, 0x43 } },
	{ '[', { 0x00, 0x7f, 0x41, 0x41, 0x00 } },
	{ '\\', { 0x02, 0x04, 0x08, 0x10, 0x20 } },
	{ ']', { 0x00, 0x41, 0x41, 0x7f, 0x00 } },
	{ '^', { 0x04, 0x02, 0x01, 0x02, 0x04 } },
	{ '_', { 0x40, 0x40, 0x40, 0x40, 0x40 } },
	{ '`', { 0x00, 0x01, 0x02, 0x04, 0x00 } },
	{ 'a', { 0x20, 0x54, 0x54, 0x54, 0x78 } },
	{ 'b', { 0x7f, 0x48, 0x44, 0x44, 0x38 } },
	{ 'c', { 0x38, 0x44, 0x44, 0x44, 0x20 } },
	{ 'd', { 0x38, 0x44, 0x44, 0x48, 0x7f } },
	{ 'e', { 0x38, 0x54, 0x54, 0x54, 0x18 } },
	{ 'f', { 0x08, 0x7e, 0x09, 0x01, 0x02 } },
	{ 'g', { 0x0c, 0x52, 0x52, 0x52, 0x3e } },
	{ 'h', { 0x7f, 0x08, 0x04, 0x04, 0x78 } },
	{ 'i', { 0x00, 0x44, 0x7d, 0x40, 0x00 } },
	{ 'j', { 0x20, 0x40, 0x44, 0x3d, 0x00 } },
	{ 'k', { 0x7f, 0x10, 0x28, 0x44, 0x00 } },
	{ 'l', { 0x00, 0x41, 0x7f, 0x40, 0x00 } },
	{ 'm', { 0x7c, 0x04, 0x18, 0x04, 0x78 } },
	{ 'n', { 0x7c, 0x08, 0x04, 0x04, 0x78 } },
	{ 'o', { 0x38, 0x44, 0x44, 0x44, 0x38 } },
	{ 'p', { 0x7c, 0x14, 0x14, 0x14, 0x08 } },
	{ 'q', { 0x08, 0x14, 0x14, 0x18, 0x7c } },
	{ 'r', { 0x7c, 0x08, 0x04, 0x04, 0x08 } },
	{ 's', { 0x48, 0x54, 0x54, 0x54, 0x20 } },
	{ 't', { 0x04, 0x3f, 0x44, 0x40, 0x20 } },
	{ 'u', { 0x3c, 0x40, 0x40, 0x20, 0x7c } },
	{ 'v', { 0x1c, 0x20, 0x40, 0x20, 0x1c } },
	{ 'w', { 0x3c, 0x40, 0x30, 0x40, 0x3c } },
	{ 'x', { 0x44, 0x28, 0x10, 0x28, 0x44 } },
	{ 'y', { 0x0c, 0x50, 0x50, 0x50, 0x3c } },
	{ 'z', { 0x44, 0x64, 0x54, 0x4c, 0x44 } },
	{ '{', { 0x00, 0x08, 0x36, 0x41, 0x00 } },
	{ '|', { 0x00, 0x00, 0x7f, 0x00, 0x00 } },
	{ '}', { 0x00, 0x41, 0x36, 0x08, 0x00 } },
	{ '~', { 0x08, 0x04, 0x08, 0x10, 0x08 } },
};

static int status_valid(enum fplinux_boot_screen_status status)
{
	return (unsigned)status <= (unsigned)FPLINUX_BOOT_SCREEN_FAILED;
}

static int screen_valid(const struct fplinux_boot_screen *screen)
{
	return screen != NULL && screen->initialized &&
	       screen->canvas.width != 0 && screen->canvas.height != 0 &&
	       screen->canvas.fill_rect != NULL &&
	       screen->canvas.present != NULL;
}

static void copy_string(char *destination, size_t capacity, const char *source)
{
	size_t i = 0;

	if (capacity == 0)
		return;
	if (source != NULL)
		while (i + 1 < capacity && source[i] != '\0') {
			destination[i] = source[i];
			++i;
		}
	destination[i] = '\0';
}

static size_t string_length(const char *text)
{
	size_t length = 0;

	if (text == NULL)
		return 0;
	while (text[length] != '\0')
		++length;
	return length;
}

static const uint8_t *glyph_for(char character)
{
	size_t i;
	const uint8_t *fallback = NULL;

	for (i = 0; i < sizeof(font) / sizeof(font[0]); ++i) {
		if (font[i].character == '?')
			fallback = font[i].columns;
		if (font[i].character == character)
			return font[i].columns;
	}
	return fallback;
}

static void fill_rect(struct fplinux_boot_screen *screen, uint32_t x,
		      uint32_t y, uint32_t width, uint32_t height,
		      uint16_t colour)
{
	if (width == 0 || height == 0 || x >= screen->canvas.width ||
	    y >= screen->canvas.height)
		return;
	if (width > screen->canvas.width - x)
		width = screen->canvas.width - x;
	if (height > screen->canvas.height - y)
		height = screen->canvas.height - y;
	if (width == 0 || height == 0)
		return;
	screen->canvas.fill_rect(screen->canvas.context, x, y, width, height,
				 colour);
}

static void draw_character(struct fplinux_boot_screen *screen, uint32_t x,
			   uint32_t y, char character, uint32_t scale,
			   uint16_t colour)
{
	const uint8_t *glyph = glyph_for(character);
	uint32_t column;
	uint32_t row;

	if (glyph == NULL || scale == 0)
		return;
	for (column = 0; column < 5; ++column)
		for (row = 0; row < 7; ++row)
			if (glyph[column] & (1u << row))
				fill_rect(screen, x + column * scale,
					  y + row * scale, scale, scale,
					  colour);
}

static uint32_t draw_text_n(struct fplinux_boot_screen *screen, uint32_t x,
			    uint32_t y, const char *text, size_t count,
			    uint32_t scale, uint16_t colour)
{
	size_t i;

	for (i = 0; i < count; ++i) {
		draw_character(screen, x, y, text[i], scale, colour);
		x += 6u * scale;
	}
	return x;
}

static uint32_t text_width(size_t count, uint32_t scale)
{
	if (count == 0 || scale == 0)
		return 0;
	return (uint32_t)count * 6u * scale - scale;
}

static size_t text_capacity(uint32_t left, uint32_t right, uint32_t scale)
{
	uint32_t available;

	if (right <= left || scale == 0)
		return 0;
	available = right - left;
	if (available < 5u * scale)
		return 0;
	return (available + scale) / (6u * scale);
}

static void draw_text_fit(struct fplinux_boot_screen *screen, uint32_t left,
			  uint32_t right, uint32_t y, const char *text,
			  uint32_t scale, uint16_t colour, size_t hard_limit)
{
	size_t capacity = text_capacity(left, right, scale);
	size_t length = string_length(text);
	size_t prefix;
	uint32_t x;

	if (text == NULL || capacity == 0)
		return;
	if (hard_limit != 0 && capacity > hard_limit)
		capacity = hard_limit;
	if (length <= capacity) {
		draw_text_n(screen, left, y, text, length, scale, colour);
		return;
	}
	if (capacity <= 3) {
		draw_text_n(screen, left, y, "...", capacity, scale, colour);
		return;
	}
	prefix = capacity - 3;
	x = draw_text_n(screen, left, y, text, prefix, scale, colour);
	draw_text_n(screen, x, y, "...", 3, scale, colour);
}

static void draw_text_right_fit(struct fplinux_boot_screen *screen,
				uint32_t left, uint32_t right, uint32_t y,
				const char *text, uint32_t scale,
				uint16_t colour)
{
	size_t capacity = text_capacity(left, right, scale);
	size_t length = string_length(text);
	size_t shown;
	uint32_t x;

	if (capacity == 0 || length == 0)
		return;
	shown = length < capacity ? length : capacity;
	x = right - text_width(shown, scale);
	draw_text_fit(screen, x, right, y, text, scale, colour, capacity);
}

static void draw_marker(struct fplinux_boot_screen *screen, uint32_t x,
			uint32_t y, enum fplinux_boot_screen_status status)
{
	uint32_t step;

	switch (status) {
	case FPLINUX_BOOT_SCREEN_ACTIVE:
		fill_rect(screen, x, y, 8, 2, COLOUR_PRIMARY);
		fill_rect(screen, x, y + 6, 8, 2, COLOUR_PRIMARY);
		fill_rect(screen, x, y + 2, 2, 4, COLOUR_PRIMARY);
		fill_rect(screen, x + 6, y + 2, 2, 4, COLOUR_PRIMARY);
		fill_rect(screen, x + 3, y + 3, 2, 2, COLOUR_PRIMARY);
		break;
	case FPLINUX_BOOT_SCREEN_DONE:
		fill_rect(screen, x, y, 8, 8, COLOUR_MUTED);
		break;
	case FPLINUX_BOOT_SCREEN_FAILED:
		for (step = 0; step < 4; ++step) {
			fill_rect(screen, x + step * 2, y + step * 2, 2, 2,
				  COLOUR_ERROR);
			fill_rect(screen, x + 6 - step * 2, y + step * 2, 2, 2,
				  COLOUR_ERROR);
		}
		break;
	case FPLINUX_BOOT_SCREEN_PENDING:
	default:
		fill_rect(screen, x, y + 3, 8, 2, COLOUR_DIM);
		break;
	}
}

static size_t visible_stage_start(const struct fplinux_boot_screen *screen,
				  size_t capacity)
{
	size_t maximum_start;

	if (capacity == 0 || screen->stage_count <= capacity)
		return 0;
	maximum_start = screen->stage_count - capacity;
	if (screen->current_stage >= screen->stage_count ||
	    screen->current_stage < capacity)
		return 0;
	if (screen->current_stage - capacity + 1 > maximum_start)
		return maximum_start;
	return screen->current_stage - capacity + 1;
}

static void format_stage_number(char number[3], size_t stage_index)
{
	size_t value = (stage_index + 1) % 100;

	number[0] = (char)('0' + value / 10);
	number[1] = (char)('0' + value % 10);
	number[2] = '\0';
}

static void draw_stage_rows(struct fplinux_boot_screen *screen,
			    uint32_t first_y, uint32_t pitch, uint32_t number_x,
			    uint32_t label_x, uint32_t marker_x,
			    size_t capacity, size_t label_limit)
{
	size_t start = visible_stage_start(screen, capacity);
	size_t row;

	for (row = 0; row < capacity && start + row < screen->stage_count;
	     ++row) {
		size_t stage = start + row;
		uint32_t y = first_y + (uint32_t)row * pitch;
		char number[3];

		format_stage_number(number, stage);
		draw_text_n(screen, number_x, y, number, 2, 2, COLOUR_MUTED);
		draw_text_fit(screen, label_x, marker_x - 8, y,
			      screen->stage_labels[stage], 2, COLOUR_PRIMARY,
			      label_limit);
		draw_marker(screen, marker_x, y + 3,
			    screen->stage_status[stage]);
	}
}

static void format_error_heading(char heading[14], uint32_t error_code)
{
	static const char prefix[] = "BOOT ERROR ";
	size_t i;

	for (i = 0; i < sizeof(prefix) - 1; ++i)
		heading[i] = prefix[i];
	if (error_code <= 99) {
		heading[11] = (char)('0' + error_code / 10);
		heading[12] = (char)('0' + error_code % 10);
	} else {
		heading[11] = '?';
		heading[12] = '?';
	}
	heading[13] = '\0';
}

static const char *copy_wrapped_line(char *line, size_t line_capacity,
				     const char *text, size_t maximum,
				     int last_line)
{
	size_t remaining;
	size_t length;
	size_t i;

	if (line_capacity == 0)
		return text;
	line[0] = '\0';
	if (text == NULL)
		return NULL;
	while (*text == ' ')
		++text;
	remaining = string_length(text);
	if (maximum >= line_capacity)
		maximum = line_capacity - 1;
	if (remaining <= maximum) {
		copy_string(line, line_capacity, text);
		return text + remaining;
	}
	if (maximum == 0)
		return text;
	if (last_line) {
		if (maximum <= 3) {
			for (i = 0; i < maximum; ++i)
				line[i] = '.';
			line[maximum] = '\0';
			return text + remaining;
		}
		for (i = 0; i < maximum - 3; ++i)
			line[i] = text[i];
		line[maximum - 3] = '.';
		line[maximum - 2] = '.';
		line[maximum - 1] = '.';
		line[maximum] = '\0';
		return text + remaining;
	}
	length = maximum;
	while (length > 0 && text[length] != ' ')
		--length;
	if (length == 0)
		length = maximum;
	for (i = 0; i < length; ++i)
		line[i] = text[i];
	line[length] = '\0';
	text += length;
	while (*text == ' ')
		++text;
	return text;
}

static void draw_wrapped_detail(struct fplinux_boot_screen *screen,
				uint32_t left, uint32_t right, uint32_t first_y,
				uint32_t line_pitch, uint32_t scale)
{
	char first[49];
	char second[49];
	size_t capacity = text_capacity(left, right, scale);
	const char *remaining;

	if (capacity == 0)
		return;
	remaining = copy_wrapped_line(first, sizeof(first),
				      screen->error_detail, capacity, 0);
	copy_wrapped_line(second, sizeof(second), remaining, capacity, 1);
	draw_text_n(screen, left, first_y, first, string_length(first), scale,
		    COLOUR_PRIMARY);
	draw_text_n(screen, left, first_y + line_pitch, second,
		    string_length(second), scale, COLOUR_PRIMARY);
}

static void draw_error_panel(struct fplinux_boot_screen *screen,
			     uint32_t panel_y, uint32_t panel_height,
			     uint32_t margin, int compact)
{
	char heading[14];
	uint32_t right = screen->canvas.width - margin;

	fill_rect(screen, 0, panel_y, screen->canvas.width, panel_height,
		  COLOUR_SURFACE);
	fill_rect(screen, 0, panel_y, 4, panel_height, COLOUR_ERROR);
	format_error_heading(heading, screen->error_code);
	if (compact) {
		draw_text_fit(screen, margin, right, panel_y + 4, heading, 1,
			      COLOUR_ERROR, 0);
		draw_wrapped_detail(screen, margin, right, panel_y + 18, 10, 1);
	} else {
		draw_text_fit(screen, margin, right, panel_y + 8, heading, 2,
			      COLOUR_ERROR, 0);
		draw_wrapped_detail(screen, margin, right, panel_y + 28, 18, 2);
	}
}

static void draw_handoff_panel(struct fplinux_boot_screen *screen,
			       uint32_t panel_y, uint32_t panel_height,
			       uint32_t margin, uint32_t marker_x, int compact)
{
	fill_rect(screen, 0, panel_y, screen->canvas.width, panel_height,
		  COLOUR_SURFACE);
	if (compact) {
		draw_text_n(screen, margin, panel_y + 5, "HANDOFF", 7, 1,
			    COLOUR_MUTED);
		draw_text_fit(screen, margin, marker_x - 8, panel_y + 23,
			      screen->status_text, 2, COLOUR_PRIMARY, 0);
		draw_marker(screen, marker_x, panel_y + 25,
			    screen->status_state);
	} else {
		draw_text_n(screen, 16, panel_y + 10, "HANDOFF", 7, 2,
			    COLOUR_MUTED);
		draw_text_fit(screen, 16, marker_x - 8, panel_y + 38,
			      screen->status_text, 2, COLOUR_PRIMARY, 0);
		draw_marker(screen, marker_x, panel_y + 41,
			    screen->status_state);
	}
}

static void render_full(struct fplinux_boot_screen *screen)
{
	uint32_t right = screen->canvas.width - 16;
	uint32_t marker_x = screen->canvas.width - 24;
	uint32_t panel_y = screen->canvas.height - 64;
	uint32_t brand_width = text_width(string_length(screen->brand), 3);
	uint32_t variant_width = text_width(string_length(screen->variant), 2);

	draw_text_fit(screen, 16, right, 14, screen->brand, 3, COLOUR_PRIMARY,
		      0);
	if (brand_width + variant_width + 3 <= right - 16)
		draw_text_right_fit(screen, 16, right, 18, screen->variant, 2,
				    COLOUR_MUTED);
	draw_text_fit(screen, 16, right, 48, screen->model, 2, COLOUR_MUTED, 0);
	fill_rect(screen, 16, 76, screen->canvas.width - 32, 1, COLOUR_RULE);
	draw_text_fit(screen, 16, right, 92, screen->mode, 2, COLOUR_PRIMARY,
		      0);
	draw_stage_rows(screen, 120, 24, 16, 48, marker_x, 5, 13);

	if (screen->has_error)
		draw_error_panel(screen, panel_y, 64, 16, 0);
	else
		draw_handoff_panel(screen, panel_y, 64, 16, marker_x, 0);
}

static void render_compact(struct fplinux_boot_screen *screen)
{
	uint32_t right = screen->canvas.width - 8;
	uint32_t marker_x = screen->canvas.width - 16;
	uint32_t panel_y = screen->canvas.height - 44;
	uint32_t first_y = 58;
	size_t capacity = 0;
	uint32_t brand_width = text_width(string_length(screen->brand), 2);
	uint32_t variant_width = text_width(string_length(screen->variant), 2);

	draw_text_fit(screen, 8, right, 8, screen->brand, 2, COLOUR_PRIMARY, 0);
	if (brand_width + variant_width + 16 <= screen->canvas.width - 16)
		draw_text_right_fit(screen, 8, right, 8, screen->variant, 2,
				    COLOUR_MUTED);
	draw_text_fit(screen, 8, right, 30, screen->mode, 2, COLOUR_PRIMARY, 0);
	fill_rect(screen, 8, 50, screen->canvas.width - 16, 1, COLOUR_RULE);
	if (panel_y >= first_y + 14)
		capacity = 1 + (panel_y - first_y - 14) / 20;
	if (capacity > FPLINUX_BOOT_SCREEN_MAX_STAGES)
		capacity = FPLINUX_BOOT_SCREEN_MAX_STAGES;
	draw_stage_rows(screen, first_y, 20, 8, 38, marker_x, capacity, 0);

	if (screen->has_error)
		draw_error_panel(screen, panel_y, 44, 8, 1);
	else
		draw_handoff_panel(screen, panel_y, 44, 8, marker_x, 1);
}

static void render_tiny(struct fplinux_boot_screen *screen)
{
	uint32_t margin = screen->canvas.width > 16 ? 8 : 0;
	uint32_t right = screen->canvas.width > margin ?
				 screen->canvas.width - margin :
				 screen->canvas.width;
	uint16_t current_colour = COLOUR_PRIMARY;
	char heading[14];

	draw_text_fit(screen, margin, right, 8, screen->brand, 2,
		      COLOUR_PRIMARY, 0);
	if (screen->has_error) {
		format_error_heading(heading, screen->error_code);
		draw_text_fit(screen, margin, right, 34, heading, 2,
			      COLOUR_ERROR, 0);
		draw_wrapped_detail(screen, margin, right, 56, 18, 2);
		return;
	}
	if (screen->current_stage >= screen->stage_count)
		return;
	if (screen->stage_status[screen->current_stage] ==
	    FPLINUX_BOOT_SCREEN_FAILED)
		current_colour = COLOUR_ERROR;
	draw_text_fit(screen, margin, right, 34,
		      screen->stage_labels[screen->current_stage], 2,
		      current_colour, 0);
}

void fplinux_boot_screen_render(struct fplinux_boot_screen *screen)
{
	if (!screen_valid(screen))
		return;
	fill_rect(screen, 0, 0, screen->canvas.width, screen->canvas.height,
		  COLOUR_BACKGROUND);
	if (screen->canvas.width >= FULL_WIDTH &&
	    screen->canvas.height >= FULL_HEIGHT)
		render_full(screen);
	else if (screen->canvas.width >= COMPACT_MIN_WIDTH &&
		 screen->canvas.height >= COMPACT_MIN_HEIGHT)
		render_compact(screen);
	else
		render_tiny(screen);
	screen->canvas.present(screen->canvas.context);
}

int fplinux_boot_screen_init(struct fplinux_boot_screen *screen,
			     const struct fplinux_boot_screen_canvas *canvas,
			     const struct fplinux_boot_screen_identity *identity,
			     const char *const *stage_labels,
			     size_t stage_count)
{
	size_t i;

	if (screen == NULL)
		return -1;
	screen->initialized = 0;
	if (canvas == NULL || canvas->width == 0 || canvas->height == 0 ||
	    canvas->fill_rect == NULL || canvas->present == NULL ||
	    stage_count > FPLINUX_BOOT_SCREEN_MAX_STAGES ||
	    (stage_count != 0 && stage_labels == NULL))
		return -1;
	screen->canvas = *canvas;
	copy_string(screen->brand, sizeof(screen->brand),
		    identity != NULL ? identity->brand : NULL);
	copy_string(screen->variant, sizeof(screen->variant),
		    identity != NULL ? identity->variant : NULL);
	copy_string(screen->model, sizeof(screen->model),
		    identity != NULL ? identity->model : NULL);
	copy_string(screen->mode, sizeof(screen->mode),
		    identity != NULL ? identity->mode : NULL);
	for (i = 0; i < FPLINUX_BOOT_SCREEN_MAX_STAGES; ++i) {
		copy_string(screen->stage_labels[i],
			    sizeof(screen->stage_labels[i]),
			    i < stage_count ? stage_labels[i] : NULL);
		screen->stage_status[i] = FPLINUX_BOOT_SCREEN_PENDING;
	}
	copy_string(screen->status_text, sizeof(screen->status_text), NULL);
	copy_string(screen->error_detail, sizeof(screen->error_detail), NULL);
	screen->status_state = FPLINUX_BOOT_SCREEN_PENDING;
	screen->stage_count = stage_count;
	screen->current_stage = (size_t)-1;
	screen->error_code = 0;
	screen->has_error = 0;
	screen->initialized = 1;
	fplinux_boot_screen_render(screen);
	return 0;
}

int fplinux_boot_screen_set_stage(struct fplinux_boot_screen *screen,
				  size_t stage_index,
				  enum fplinux_boot_screen_status status)
{
	if (!screen_valid(screen) || stage_index >= screen->stage_count ||
	    !status_valid(status) || screen->has_error)
		return -1;
	screen->stage_status[stage_index] = status;
	if (status != FPLINUX_BOOT_SCREEN_PENDING)
		screen->current_stage = stage_index;
	fplinux_boot_screen_render(screen);
	return 0;
}

int fplinux_boot_screen_set_checkpoint(struct fplinux_boot_screen *screen,
				       const char *text,
				       enum fplinux_boot_screen_status status)
{
	if (!screen_valid(screen) || !status_valid(status) || screen->has_error)
		return -1;
	copy_string(screen->status_text, sizeof(screen->status_text), text);
	screen->status_state = status;
	fplinux_boot_screen_render(screen);
	return 0;
}

void fplinux_boot_screen_fail(struct fplinux_boot_screen *screen,
			      uint32_t error_code, const char *detail)
{
	if (!screen_valid(screen))
		return;
	if (screen->current_stage < screen->stage_count)
		screen->stage_status[screen->current_stage] =
			FPLINUX_BOOT_SCREEN_FAILED;
	copy_string(screen->error_detail, sizeof(screen->error_detail), detail);
	screen->error_code = error_code;
	screen->has_error = 1;
	fplinux_boot_screen_render(screen);
}
