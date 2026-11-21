/* SPDX-License-Identifier: 0BSD */
/* The same screen commands supply the legends in both display modes. */
#include "apidemo.h"

enum {
	COMMAND_CHANGE = 1,
	COMMAND_FULLSCREEN,
};

static struct {
	struct fplinux_ui_screen *canvas;
	lv_obj_t *scroll;
	bool changed;
} preview;

static const char *change_label(void)
{
	return preview.changed ? _("Restore") : _("Change");
}

/* OK changes the legends: its own command, whose label says so, included. */
static void add_change(struct apidemo *app)
{
	const struct fplinux_ui_command change = {
		.id = COMMAND_CHANGE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = change_label(),
		.icon = apidemo_glyph(app, "edit"),
	};

	fplinux_ui_screen_add_command(preview.canvas, &change);
}

/*
 * Refit after a surface resize, which a change of display mode also brings.
 */
static void layout_preview(struct apidemo *app)
{
	int width;
	int height;

	if (!preview.scroll)
		return;
	if (!fplinux_ui_screen_get_content_size(preview.canvas, &width,
						&height)) {
		apidemo_error(app, "fplinux_ui_screen_get_content_size");
		return;
	}
	lv_obj_set_size(preview.scroll, width, height);
}

static void preview_deleted(lv_event_t *event)
{
	(void)event;
	preview.canvas = NULL;
	preview.scroll = NULL;
}

static void preview_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
			const struct fplinux_ui_key *key, void *context)
{
	(void)ui;
	(void)canvas;
	(void)context;
	if (preview.scroll)
		fplinux_ui_scroll_key(preview.scroll, key);
}

static void preview_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *screen, int command_id,
			    void *context)
{
	struct apidemo *app = context;

	(void)screen;
	if (command_id == APIDEMO_COMMAND_BACK) {
		/* Back leaves full screen first, then the screen. */
		if (!fplinux_ui_is_fullscreen(ui))
			fplinux_ui_go_back(ui);
		else if (!fplinux_ui_set_fullscreen(ui, false))
			apidemo_error(app, "fplinux_ui_set_fullscreen");
	} else if (command_id == COMMAND_FULLSCREEN) {
		if (!fplinux_ui_set_fullscreen(ui,
					       !fplinux_ui_is_fullscreen(ui)))
			apidemo_error(app, "fplinux_ui_set_fullscreen");
	} else if (command_id == COMMAND_CHANGE) {
		preview.changed = !preview.changed;
		add_change(app);
	}
}

static void open_preview(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *canvas =
		fplinux_ui_canvas_new(app->ui, _(entry->title));
	const struct fplinux_ui_command mode = {
		.id = COMMAND_FULLSCREEN,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Toggle full screen"),
		.icon = apidemo_glyph(app, "fullscreen"),
	};
	lv_obj_t *area;
	lv_obj_t *hint;

	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	area = fplinux_ui_canvas_object(canvas);
	preview.canvas = canvas;
	preview.changed = false;
	fplinux_ui_canvas_set_fullscreen_bar(canvas, true);
	lv_obj_add_event_cb(area, preview_deleted, LV_EVENT_DELETE, NULL);
	preview.scroll = lv_obj_create(area);
	lv_obj_remove_style_all(preview.scroll);
	lv_obj_set_style_pad_hor(preview.scroll, app->metrics->margin, 0);
	lv_obj_set_style_pad_ver(preview.scroll, app->metrics->spacing, 0);
	apidemo_style_scroll(app, preview.scroll);
	hint = lv_label_create(preview.scroll);
	lv_obj_set_width(hint, lv_pct(100));
	lv_obj_set_style_text_font(hint, app->theme->font, 0);
	lv_obj_set_style_text_color(hint, app->theme->foreground, 0);
	lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(
		hint,
		_("Options > Toggle full screen uses the same commands in both "
		  "views. Change and Restore switch the centre label. Back "
		  "leaves full screen, then returns to the catalogue."));
	layout_preview(app);
	apidemo_add_back(canvas);
	fplinux_ui_screen_add_command(canvas, &mode);
	add_change(app);
	fplinux_ui_screen_set_listener(canvas, preview_command, NULL, app);
	fplinux_ui_canvas_set_key_handler(canvas, preview_key, NULL);
	fplinux_ui_canvas_set_key_repeat(canvas,
					 FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
	layout_preview(app);
}

static const struct apidemo_entry entries[] = {
	{ N_("Soft-key preview"), "fplinux_ui_canvas_set_fullscreen_bar",
	  open_preview, 0, N_("Change legends and toggle full screen") },
};

const struct apidemo_category apidemo_softkeys = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.resize = layout_preview,
};
