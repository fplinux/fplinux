/* SPDX-License-Identifier: 0BSD */
/*
 * Look: the theme's colours and fonts, the profile's metrics, the system
 * glyphs, decoded images and text that does not fit.
 *
 * The colour, font, glyph and image pages are canvases: screens the
 * application draws with LVGL, laid out only from fplinux_ui_metrics(),
 * fplinux_ui_theme() and the surface size, and scrolled with
 * fplinux_ui_scroll_key().
 */
/* open(), fstat() and read() of POSIX.1-2008. */
#define _POSIX_C_SOURCE 200809L
#include "apidemo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEXT_BYTES 256
/* The alpha-only ring that the image demo draws in memory. */
#define RING_PIXELS 48

/* Pages */

/* Up and Down scroll the page by a line, Left and Right by a page. */
static void page_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		     const struct fplinux_ui_key *key, void *context)
{
	(void)ui;
	(void)canvas;
	if (key->pressed)
		fplinux_ui_scroll_key(context, key);
}

/*
 * A canvas whose drawing area holds one scrolling column. The column's
 * children take its width and stack with the profile's spacing. The
 * viewport owns both margins and the conditional scroll indicator.
 */
static lv_obj_t *new_page(struct apidemo *app, const char *title,
			  struct fplinux_ui_screen **canvas)
{
	const struct fplinux_ui_metrics *metrics = app->metrics;
	lv_obj_t *page;

	*canvas = fplinux_ui_canvas_new(app->ui, title);
	if (!*canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return NULL;
	}
	page = lv_obj_create(fplinux_ui_canvas_object(*canvas));
	lv_obj_remove_style_all(page);
	lv_obj_set_size(page, lv_pct(100), lv_pct(100));
	lv_obj_set_style_pad_hor(page, metrics->margin, 0);
	lv_obj_set_style_pad_ver(page, metrics->spacing, 0);
	lv_obj_set_style_pad_row(page, metrics->spacing, 0);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	apidemo_style_scroll(app, page);
	apidemo_add_back(*canvas);
	fplinux_ui_screen_set_listener(*canvas, apidemo_back_listener, NULL,
				       app);
	fplinux_ui_canvas_set_key_handler(*canvas, page_key, page);
	fplinux_ui_canvas_set_key_repeat(*canvas,
					 FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
	return page;
}

/*
 * A wrapping label fills the viewport's content width.
 */
static lv_obj_t *add_text(lv_obj_t *parent, const lv_font_t *font,
			  lv_color_t color, const char *text)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, color, 0);
	lv_label_set_text(label, text);
	return label;
}

/* A row of the page: children side by side, centred vertically. */
static lv_obj_t *add_row(struct apidemo *app, lv_obj_t *page)
{
	lv_obj_t *row = lv_obj_create(page);

	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_column(row, app->metrics->spacing, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	return row;
}

/* Theme colours */

struct colour_role {
	/* The field of struct fplinux_ui_theme, and what it is for. */
	const char *name;
	const char *role;
	lv_color_t swatch;
	/*
	 * A text colour on the page is shown by the name itself; a background
	 * by a glyph in the text colour that goes on it, inside the swatch.
	 */
	lv_color_t text;
	bool background;
};

/*
 * A swatch of the colour beside the field's name; what the colour is for
 * follows below, in muted. Pairs: foreground on background, surface and
 * popup, accent_text on accent, bar_text on bar.
 */
static void add_colour(struct apidemo *app, lv_obj_t *page,
		       const struct colour_role *colour)
{
	const struct fplinux_ui_metrics *metrics = app->metrics;
	lv_obj_t *row = add_row(app, page);
	lv_obj_t *swatch = lv_obj_create(row);
	lv_obj_t *name;

	lv_obj_remove_style_all(swatch);
	lv_obj_set_size(swatch, metrics->row_icon_size, metrics->row_icon_size);
	lv_obj_set_style_bg_color(swatch, colour->swatch, 0);
	lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
	/* A border, so that the background swatch shows on the page. */
	lv_obj_set_style_border_color(swatch, app->theme->border, 0);
	lv_obj_set_style_border_width(swatch, 1, 0);
	if (colour->background) {
		lv_obj_t *glyph = lv_image_create(swatch);

		lv_image_set_src(glyph,
				 fplinux_ui_glyph(app->ui, "edit",
						  FPLINUX_UI_GLYPH_STATUS));
		lv_obj_set_style_image_recolor(glyph, colour->text, 0);
		lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
		lv_obj_center(glyph);
	}
	name = lv_label_create(row);
	lv_obj_set_style_text_font(name, app->theme->font, 0);
	lv_obj_set_style_text_color(
		name,
		colour->background ? app->theme->foreground : colour->text, 0);
	lv_label_set_text(name, colour->name);
	add_text(page, app->theme->font, app->theme->muted, _(colour->role));
}

static void open_colours(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_theme *theme = app->theme;
	const struct colour_role colours[] = {
		{ "background", N_("The page"), theme->background,
		  theme->foreground, true },
		{ "surface", N_("Title bands, form and field frames"),
		  theme->surface, theme->foreground, true },
		{ "popup", N_("Dialogs, the options menu, the symbol grid"),
		  theme->popup, theme->foreground, true },
		{ "foreground", N_("Text and glyphs"), theme->foreground,
		  theme->foreground, false },
		{ "muted", N_("Secondary text: details, labels, hints"),
		  theme->muted, theme->muted, false },
		{ "disabled", N_("Unavailable items, placeholders"),
		  theme->disabled, theme->disabled, false },
		{ "border", N_("Dividers, unfocused frames, tracks"),
		  theme->border, theme->foreground, false },
		{ "accent", N_("The selected row or cell"), theme->accent,
		  theme->accent_text, true },
		{ "accent_text", N_("Text and glyphs on accent"),
		  theme->accent_text, theme->foreground, false },
		{ "bar", N_("The status and soft-key bars"), theme->bar,
		  theme->bar_text, true },
		{ "bar_text", N_("Text and glyphs on bar"), theme->bar_text,
		  theme->foreground, false },
		{ "focus", N_("Focus rings, gauges, spinners, scrolling"),
		  theme->focus, theme->foreground, false },
		{ "warning", N_("The frame and title of a warning"),
		  theme->warning, theme->warning, false },
		{ "danger", N_("The frame and title of an error"),
		  theme->danger, theme->danger, false },
		{ "tile_ring", N_("The ring around a tile on accent"),
		  theme->tile_ring, theme->foreground, false },
		{ "brand", N_("Decoration only, never text"), theme->brand,
		  theme->foreground, false },
		{ "scrim", N_("Dimming below a modal"), theme->scrim,
		  theme->foreground, false },
	};
	struct fplinux_ui_screen *canvas;
	lv_obj_t *page = new_page(app, _(entry->title), &canvas);
	char opacity[64];
	const char *kind;

	if (!page)
		return;
	switch (theme->kind) {
	case FPLINUX_UI_THEME_DARK:
		kind = _("The dark theme, chosen in Settings.");
		break;
	case FPLINUX_UI_THEME_LIGHT:
		kind = _("The light theme, chosen in Settings.");
		break;
	default:
		/* A later release may add a theme. */
		kind = _("A theme this demo does not know.");
		break;
	}
	add_text(page, theme->font, theme->foreground, kind);
	for (size_t i = 0; i < APIDEMO_COUNT(colours); i++)
		add_colour(app, page, &colours[i]);
	snprintf(opacity, sizeof(opacity), "scrim_opacity: %u/255",
		 (unsigned int)theme->scrim_opacity);
	add_text(page, theme->font, theme->foreground, opacity);
	fplinux_ui_push(app->ui, canvas);
}

/* Theme fonts */

static void add_font(struct apidemo *app, lv_obj_t *page, const char *name,
		     const lv_font_t *font, const char *sample)
{
	char text[TEXT_BYTES];

	add_text(page, font, app->theme->foreground, sample);
	/* TRANSLATORS: %1$s is a field of the theme, %2$d pixels. */
	snprintf(text, sizeof(text), _("%1$s, lines of %2$d px"), name,
		 (int)lv_font_get_line_height(font));
	add_text(page, app->theme->font, app->theme->muted, text);
}

static void open_fonts(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_theme *theme = app->theme;
	struct fplinux_ui_screen *canvas;
	lv_obj_t *page = new_page(app, _(entry->title), &canvas);

	if (!page)
		return;
	/* Latin, Cyrillic and the signs the theme fonts cover. */
	add_font(app, page, "font", theme->font,
		 _("Body text: rows, values, dialogs. Aa Бб Ёё 0123 № € ₽ ✓ "
		   "← ↑ → ↓"));
	add_font(app, page, "font_large", theme->font_large,
		 _("Titles and emphasis"));
	/* The clock font has digits, space and + , - . : E e only. */
	add_font(app, page, "font_clock", theme->font_clock, "12:05 -3.5");
	add_font(app, page, "font_softkey", theme->font_softkey,
		 _("Legends a fullscreen application draws"));
	fplinux_ui_push(app->ui, canvas);
}

/* Metrics */

static void add_metric(struct fplinux_ui_screen *form, const char *name,
		       int value)
{
	char text[TEXT_BYTES];

	/* TRANSLATORS: a size in pixels. */
	snprintf(text, sizeof(text), _("%d px"), value);
	fplinux_ui_form_append_string(form, name, text);
}

static void open_metrics(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_metrics *metrics = app->metrics;
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));
	char text[TEXT_BYTES];
	const char *profile;

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	switch (metrics->profile) {
	case FPLINUX_UI_PROFILE_128X128:
		profile = "FPLINUX_UI_PROFILE_128X128";
		break;
	case FPLINUX_UI_PROFILE_128X160:
		profile = "FPLINUX_UI_PROFILE_128X160";
		break;
	case FPLINUX_UI_PROFILE_176X220:
		profile = "FPLINUX_UI_PROFILE_176X220";
		break;
	case FPLINUX_UI_PROFILE_240X320:
		profile = "FPLINUX_UI_PROFILE_240X320";
		break;
	case FPLINUX_UI_PROFILE_320X240:
		profile = "FPLINUX_UI_PROFILE_320X240";
		break;
	default:
		/* Later versions may add profiles: compare sizes instead. */
		profile = _("A profile this demo does not know");
		break;
	}
	fplinux_ui_form_append_string(form, "profile", profile);
	/* TRANSLATORS: width x height in pixels. */
	snprintf(text, sizeof(text), _("%d × %d px"), fplinux_ui_width(app->ui),
		 fplinux_ui_height(app->ui));
	fplinux_ui_form_append_string(form, "fplinux_ui_width × height", text);
	snprintf(text, sizeof(text), _("%d × %d px"), metrics->screen_width,
		 metrics->screen_height);
	fplinux_ui_form_append_string(form, "screen_width × screen_height",
				      text);
	add_metric(form, "status_height", metrics->status_height);
	add_metric(form, "softkey_height", metrics->softkey_height);
	add_metric(form, "header_height", metrics->header_height);
	add_metric(form, "margin", metrics->margin);
	add_metric(form, "scrollbar_space", metrics->scrollbar_space);
	add_metric(form, "spacing", metrics->spacing);
	add_metric(form, "gap_small", metrics->gap_small);
	add_metric(form, "row_height", metrics->row_height);
	add_metric(form, "row_height_detail", metrics->row_height_detail);
	add_metric(form, "row_icon_size", metrics->row_icon_size);
	add_metric(form, "status_icon_size", metrics->status_icon_size);
	add_metric(form, "app_icon_size", metrics->app_icon_size);
	snprintf(text, sizeof(text), "%d", metrics->grid_columns);
	fplinux_ui_form_append_string(form, "grid_columns", text);
	add_metric(form, "focus_width", metrics->focus_width);
	add_metric(form, "frame_padding", metrics->frame_padding);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, apidemo_back_listener, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* System glyphs */

struct glyph {
	const char *name;
	const char *meaning;
};

/* The glyph names exposed by the UI runtime. */
static const struct glyph glyphs[] = {
	{ "lock", N_("The keypad is locked") },
	{ "alarm", N_("An alarm is set") },
	{ "no-sim", N_("No SIM card") },
	{ "save", N_("Save") },
	{ "send", N_("Send") },
	{ "delete", N_("Delete stored data") },
	{ "edit", N_("Edit") },
	{ "help", N_("Help") },
	{ "search", N_("Search") },
	{ "add", N_("Add a new item") },
	{ "display", N_("Display settings") },
	{ "brightness", N_("Brightness") },
	{ "screen-timeout", N_("When the screen turns off") },
	{ "auto-lock", N_("When the keypad locks") },
	{ "theme", N_("Light or dark theme") },
	{ "language", N_("Language") },
	{ "date-time", N_("Date and time") },
	{ "about", N_("Information about something") },
	{ "app", N_("An application without an icon") },
};

/* An lv_image draws an alpha-only glyph in its image_recolor colour. */
static void add_glyph_image(lv_obj_t *parent, const void *glyph,
			    lv_color_t color)
{
	lv_obj_t *image = lv_image_create(parent);

	lv_image_set_src(image, glyph);
	lv_obj_set_style_image_recolor(image, color, 0);
	lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
}

/*
 * One glyph at both sizes, in the colours of its contexts: foreground on
 * the page, accent_text on a selected row, bar_text on a bar and danger.
 */
static void open_glyph_sizes(struct apidemo *app, const struct glyph *glyph)
{
	const struct fplinux_ui_theme *theme = app->theme;
	const enum fplinux_ui_glyph_size sizes[] = {
		FPLINUX_UI_GLYPH_ROW,
		FPLINUX_UI_GLYPH_STATUS,
	};
	const char *size_names[] = {
		"FPLINUX_UI_GLYPH_ROW",
		"FPLINUX_UI_GLYPH_STATUS",
	};
	struct fplinux_ui_screen *canvas;
	lv_obj_t *page = new_page(app, glyph->name, &canvas);

	if (!page)
		return;
	add_text(page, theme->font, theme->foreground, _(glyph->meaning));
	for (size_t i = 0; i < APIDEMO_COUNT(sizes); i++) {
		/* The runtime keeps each glyph until fplinux_ui_close(). */
		const void *image =
			fplinux_ui_glyph(app->ui, glyph->name, sizes[i]);
		lv_obj_t *row;
		lv_obj_t *chip;

		add_text(page, theme->font, theme->muted, size_names[i]);
		if (!image) {
			add_text(page, theme->font, theme->danger,
				 apidemo_errno_name(errno));
			continue;
		}
		row = add_row(app, page);
		add_glyph_image(row, image, theme->foreground);
		add_glyph_image(row, image, theme->danger);
		/* On accent and on bar, in the text colour that goes there. */
		chip = add_row(app, row);
		lv_obj_set_width(chip, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_color(chip, theme->accent, 0);
		lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_all(chip, app->metrics->gap_small, 0);
		add_glyph_image(chip, image, theme->accent_text);
		chip = add_row(app, row);
		lv_obj_set_width(chip, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_color(chip, theme->bar, 0);
		lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_all(chip, app->metrics->gap_small, 0);
		add_glyph_image(chip, image, theme->bar_text);
	}
	fplinux_ui_push(app->ui, canvas);
}

static void glyph_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			  int command_id, void *context)
{
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND &&
		 row < APIDEMO_COUNT(glyphs))
		open_glyph_sizes(context, &glyphs[row]);
}

/* A list row draws a glyph in its text colour, accent_text when focused. */
static void open_glyphs(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_IMPLICIT);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(glyphs); i++) {
		size_t row = fplinux_ui_list_append(list, _(glyphs[i].meaning),
						    glyphs[i].name);

		if (row != SIZE_MAX)
			fplinux_ui_list_set_icon(
				list, row, apidemo_glyph(app, glyphs[i].name),
				FPLINUX_UI_LIST_ICON_IMAGE);
	}
	fplinux_ui_list_set_select_label(list, _("Open"));
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, glyph_command, NULL, app);
	fplinux_ui_push(app->ui, list);
}

/* Images */

/* An image object owns the buffer it shows: free it after the object. */
static void image_deleted(lv_event_t *event)
{
	lv_draw_buf_destroy(lv_event_get_user_data(event));
}

/*
 * Shows a buffer of fplinux_ui_image_load() under a caption naming the call
 * and the size it decoded to, or the errno of a failed call.
 */
static void add_image(struct apidemo *app, lv_obj_t *page, const char *call,
		      lv_draw_buf_t *buffer, int error, const lv_color_t *tint)
{
	char text[TEXT_BYTES];
	lv_obj_t *image;

	if (!buffer) {
		snprintf(text, sizeof(text), "%s: NULL, %s", call,
			 apidemo_errno_name(error));
		add_text(page, app->theme->font, app->theme->muted, text);
		return;
	}
	snprintf(text, sizeof(text), "%s: %u × %u", call,
		 (unsigned int)buffer->header.w,
		 (unsigned int)buffer->header.h);
	add_text(page, app->theme->font, app->theme->muted, text);
	image = lv_image_create(page);
	lv_image_set_src(image, buffer);
	lv_obj_add_event_cb(image, image_deleted, LV_EVENT_DELETE, buffer);
	/* An alpha-only buffer is drawn in its image_recolor colour. */
	if (tint) {
		lv_obj_set_style_image_recolor(image, *tint, 0);
		lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
	}
}

/* Loads src at width x height and shows the result. */
static void load_and_add(struct apidemo *app, lv_obj_t *page, const char *call,
			 const void *src, int width, int height,
			 const lv_color_t *tint)
{
	lv_draw_buf_t *buffer;

	errno = 0;
	buffer = fplinux_ui_image_load(src, width, height);
	add_image(app, page, call, buffer, errno, tint);
}

/*
 * An alpha-only ring drawn in memory: an lv_image_dsc_t in
 * LV_COLOR_FORMAT_A8 is decoded like a "-symbolic.png" file.
 */
static uint8_t ring_pixels[RING_PIXELS * RING_PIXELS];
static const lv_image_dsc_t ring = {
	.header = {
		.magic = LV_IMAGE_HEADER_MAGIC,
		.cf = LV_COLOR_FORMAT_A8,
		.w = RING_PIXELS,
		.h = RING_PIXELS,
		.stride = RING_PIXELS,
	},
	.data_size = sizeof(ring_pixels),
	.data = ring_pixels,
};

static void draw_ring(void)
{
	const int centre = RING_PIXELS / 2;
	const int outer = centre * centre;
	const int inner = (centre * 2 / 3) * (centre * 2 / 3);

	for (int y = 0; y < RING_PIXELS; y++)
		for (int x = 0; x < RING_PIXELS; x++) {
			int dx = x - centre;
			int dy = y - centre;
			int distance = dx * dx + dy * dy;

			ring_pixels[y * RING_PIXELS + x] =
				distance < outer && distance >= inner ?
					LV_OPA_COVER :
					LV_OPA_TRANSP;
		}
}

/*
 * PNG data in memory, as a program carries it compiled in: here read from
 * the package's file. The descriptor names the PNG's size from its header.
 */
static uint8_t *read_file(const char *path, size_t *size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat status;
	uint8_t *data = NULL;
	ssize_t got;

	if (fd < 0)
		return NULL;
	if (fstat(fd, &status) == 0 && status.st_size > 0) {
		data = malloc((size_t)status.st_size);
		got = data ? read(fd, data, (size_t)status.st_size) : -1;
		if (got != status.st_size) {
			free(data);
			data = NULL;
			errno = EIO;
		} else {
			*size = (size_t)got;
		}
	}
	close(fd);
	return data;
}

static uint32_t png_number(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | bytes[3];
}

static void add_png_data(struct apidemo *app, lv_obj_t *page, int width,
			 int height)
{
	size_t size = 0;
	uint8_t *data = read_file(APIDEMO_PHOTO, &size);
	lv_image_dsc_t png;
	lv_draw_buf_t *buffer;
	int error;

	if (!data || size < 24) {
		add_image(app, page, "lv_image_dsc_t PNG", NULL,
			  data ? EINVAL : errno, NULL);
		free(data);
		return;
	}
	/* Width and height are the first fields of the IHDR chunk. */
	png = (lv_image_dsc_t){
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_RAW_ALPHA,
			.w = png_number(data + 16),
			.h = png_number(data + 20),
		},
		.data_size = (uint32_t)size,
		.data = data,
	};
	errno = 0;
	buffer = fplinux_ui_image_load(&png, width, height);
	error = errno;
	/* The buffer is decoded: the PNG data is no longer needed. */
	free(data);
	add_image(app, page, "lv_image_dsc_t PNG", buffer, error, NULL);
}

static void open_images(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_metrics *metrics = app->metrics;
	const struct fplinux_ui_theme *theme = app->theme;
	struct fplinux_ui_screen *canvas;
	lv_obj_t *page = new_page(app, _(entry->title), &canvas);

	if (!page)
		return;
	load_and_add(app, page, "photo.png, app_icon_size", APIDEMO_PHOTO,
		     metrics->app_icon_size, metrics->app_icon_size, NULL);
	load_and_add(app, page, "photo.png, row_icon_size", APIDEMO_PHOTO,
		     metrics->row_icon_size, metrics->row_icon_size, NULL);
	/* "-symbolic.png": alpha only, drawn in the colour of its context. */
	load_and_add(app, page, "star-symbolic.png", APIDEMO_SHAPE,
		     metrics->app_icon_size, metrics->app_icon_size,
		     &theme->foreground);
	load_and_add(app, page, "star-symbolic.png, focus", APIDEMO_SHAPE,
		     metrics->row_icon_size, metrics->row_icon_size,
		     &theme->focus);
	draw_ring();
	load_and_add(app, page, "lv_image_dsc_t A8", &ring,
		     metrics->app_icon_size, metrics->app_icon_size,
		     &theme->danger);
	add_png_data(app, page, metrics->app_icon_size, metrics->app_icon_size);
	fplinux_ui_push(app->ui, canvas);
}

static const struct apidemo_entry entries[] = {
	{ N_("Theme colours"),
	  "fplinux_ui_theme · background · surface · popup · accent · scrim",
	  open_colours, 0, N_("Compare the current theme roles") },
	{ N_("Theme fonts"),
	  "font · font_large · font_clock · font_softkey · lv_font_get_line_height",
	  open_fonts, 0, N_("Read samples in each system font") },
	{ N_("Metrics"),
	  "fplinux_ui_metrics · fplinux_ui_width · fplinux_ui_height · profile",
	  open_metrics, 0, N_("Inspect the active screen profile") },
	{ N_("System glyphs"),
	  "fplinux_ui_glyph · FPLINUX_UI_GLYPH_ROW · FPLINUX_UI_GLYPH_STATUS",
	  open_glyphs, 0, N_("View system icons at their supported sizes") },
	{ N_("Images"),
	  "fplinux_ui_image_load · LV_COLOR_FORMAT_A8 · lv_draw_buf_destroy",
	  open_images, 0, N_("Load photos and recolour transparent shapes") },
};

const struct apidemo_category apidemo_look = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
};
