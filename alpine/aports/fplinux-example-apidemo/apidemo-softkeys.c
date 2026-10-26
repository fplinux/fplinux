/* SPDX-License-Identifier: 0BSD */
/* Application-owned legends, with the runtime's geometry and bar appearance. */
#include "apidemo.h"

#include <errno.h>
#include <stdio.h>

enum {
	COMMAND_CHANGE = 1,
	COMMAND_FULLSCREEN,
};

static struct {
	struct fplinux_ui_screen *canvas;
	lv_obj_t *bar;
	lv_obj_t *scroll;
	bool changed;
} preview;

/* Refit after a surface resize, and after the live example changes its keys. */
static void layout_preview(struct apidemo *app)
{
	int width;
	int height;

	if (!preview.bar)
		return;
	if (!fplinux_ui_screen_get_content_size(preview.canvas, &width,
						&height)) {
		apidemo_error(app, "fplinux_ui_screen_get_content_size");
		return;
	}
	if (!fplinux_ui_softkey_bar_set(
		    app->ui, preview.bar, NULL,
		    preview.changed ? _("Stop") : _("Start"), _("Back")))
		apidemo_error(app, "fplinux_ui_softkey_bar_set");
	lv_obj_set_size(preview.scroll, width,
			height - app->metrics->softkey_height);
}

static void preview_deleted(lv_event_t *event)
{
	(void)event;
	preview.canvas = NULL;
	preview.bar = NULL;
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
		if (!fplinux_ui_set_fullscreen(ui, false))
			perror("fplinux_ui_set_fullscreen");
		fplinux_ui_go_back(ui);
	} else if (command_id == COMMAND_FULLSCREEN) {
		if (!fplinux_ui_set_fullscreen(ui,
					       !fplinux_ui_is_fullscreen(ui)))
			apidemo_error(app, "fplinux_ui_set_fullscreen");
	} else if (command_id == COMMAND_CHANGE) {
		preview.changed = !preview.changed;
		layout_preview(app);
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
	const struct fplinux_ui_command change = {
		.id = COMMAND_CHANGE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Change legends"),
		.icon = apidemo_glyph(app, "edit"),
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
	preview.bar = fplinux_ui_softkey_bar_create(app->ui, area);
	if (!preview.bar) {
		int saved = errno;

		fplinux_ui_push(app->ui, canvas);
		fplinux_ui_pop(app->ui);
		errno = saved;
		apidemo_error(app, "fplinux_ui_softkey_bar_create");
		return;
	}
	lv_obj_add_event_cb(preview.bar, preview_deleted, LV_EVENT_DELETE,
			    NULL);
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
		_("Change legends updates this preview. Toggle full screen "
		  "changes the display mode; Back returns."));
	layout_preview(app);
	apidemo_add_back(canvas);
	fplinux_ui_screen_add_command(canvas, &mode);
	fplinux_ui_screen_add_command(canvas, &change);
	fplinux_ui_screen_set_listener(canvas, preview_command, NULL, app);
	fplinux_ui_canvas_set_key_handler(canvas, preview_key, NULL);
	fplinux_ui_canvas_set_key_repeat(canvas,
					 FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
	layout_preview(app);
}

static const struct apidemo_entry entries[] = {
	{ N_("Soft-key preview"),
	  "fplinux_ui_softkey_bar_create · fplinux_ui_softkey_bar_set",
	  open_preview, 0, N_("Change legends and toggle full screen") },
};

const struct apidemo_category apidemo_softkeys = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.resize = layout_preview,
};
