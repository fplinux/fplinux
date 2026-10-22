/* SPDX-License-Identifier: 0BSD */
/* Application-owned legends, with the runtime's geometry and bar appearance. */
#include "apidemo.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

enum {
	COMMAND_CHANGE = 1,
	COMMAND_FULLSCREEN,
	VARIANT_LONG = 8,
	VARIANT_CHANGE,
	VARIANT_TOO_LONG,
	VARIANT_CONTROL,
	VARIANT_NULL_PARENT,
};

static struct {
	lv_obj_t *bar;
	lv_obj_t *hint;
	int variant;
	bool changed;
} preview;

/* Refit after a surface resize, and after the live example changes its keys. */
static void layout_preview(struct apidemo *app)
{
	const char *left = preview.variant & 1 ? _("Options") : NULL;
	const char *centre = preview.variant & 2 ? _("Select") : "";
	const char *right = preview.variant & 4 ? _("Back") : NULL;

	if (!preview.bar)
		return;
	if (preview.variant == VARIANT_LONG) {
		left = _("Application settings");
		centre = _("Select application");
		right = _("Return to catalogue");
	} else if (preview.variant == VARIANT_CHANGE) {
		left = NULL;
		centre = preview.changed ? _("Stop") : _("Start");
		right = _("Back");
	} else if (preview.variant > VARIANT_CHANGE) {
		left = _("Options");
		centre = _("Select");
		right = _("Back");
	}
	if (!fplinux_ui_softkey_bar_set(app->ui, preview.bar, left, centre,
					right))
		apidemo_error(app, "fplinux_ui_softkey_bar_set");
	lv_obj_set_width(preview.hint,
			 fplinux_ui_width(app->ui) - 2 * app->metrics->margin);
}

static void preview_deleted(lv_event_t *event)
{
	(void)event;
	preview.bar = NULL;
	preview.hint = NULL;
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
	};
	const struct fplinux_ui_command change = {
		.id = COMMAND_CHANGE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Change legends"),
	};
	lv_obj_t *area;
	int error = 0;
	char too_long[129];

	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	area = fplinux_ui_canvas_object(canvas);
	preview.variant = entry->variant;
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
	preview.hint = lv_label_create(area);
	lv_obj_set_pos(preview.hint, app->metrics->margin,
		       app->metrics->spacing);
	lv_obj_set_style_text_font(preview.hint, app->theme->font, 0);
	lv_obj_set_style_text_color(preview.hint, app->theme->foreground, 0);
	lv_label_set_long_mode(preview.hint, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(preview.hint,
			  _("Preview of application legends. "
			    "Options switches full screen; Back returns."));
	layout_preview(app);
	/* Failed updates preserve the three visible labels. */
	if (entry->variant == VARIANT_TOO_LONG) {
		memset(too_long, 'x', sizeof(too_long) - 1);
		too_long[sizeof(too_long) - 1] = '\0';
		if (!fplinux_ui_softkey_bar_set(app->ui, preview.bar, too_long,
						"Select", "Back"))
			error = errno;
	} else if (entry->variant == VARIANT_CONTROL) {
		if (!fplinux_ui_softkey_bar_set(app->ui, preview.bar, "Options",
						"line\nbreak", "Back"))
			error = errno;
	} else if (entry->variant == VARIANT_NULL_PARENT) {
		if (!fplinux_ui_softkey_bar_create(app->ui, NULL))
			error = errno;
	}
	if (error)
		lv_label_set_text_fmt(
			preview.hint,
			_("Rejected: %s. Legends stay unchanged."),
			apidemo_errno_name(error));
	apidemo_add_back(canvas);
	fplinux_ui_screen_add_command(canvas, &mode);
	if (entry->variant == VARIANT_CHANGE)
		fplinux_ui_screen_add_command(canvas, &change);
	fplinux_ui_screen_set_listener(canvas, preview_command, NULL, app);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
	layout_preview(app);
}

#define LEGEND_DEMO(title, variant)                                     \
	{ N_(title),                                                    \
	  "fplinux_ui_softkey_bar_create · fplinux_ui_softkey_bar_set", \
	  open_preview, variant }

static const struct apidemo_entry entries[] = {
	LEGEND_DEMO("Empty legends", 0),
	LEGEND_DEMO("Left legend", 1),
	LEGEND_DEMO("Centre legend", 2),
	LEGEND_DEMO("Left and centre legends", 3),
	LEGEND_DEMO("Right legend", 4),
	LEGEND_DEMO("Left and right legends", 5),
	LEGEND_DEMO("Centre and right legends", 6),
	LEGEND_DEMO("Three legends", 7),
	LEGEND_DEMO("Long legends", VARIANT_LONG),
	LEGEND_DEMO("Changing legends", VARIANT_CHANGE),
	LEGEND_DEMO("Legend byte limit", VARIANT_TOO_LONG),
	LEGEND_DEMO("Legend control character", VARIANT_CONTROL),
	LEGEND_DEMO("Missing bar parent", VARIANT_NULL_PARENT),
};

const struct apidemo_category apidemo_softkeys = {
	.title = N_("Soft-key bar"),
	.icon = APIDEMO_IMAGE_DIR "commands-symbolic.png",
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.resize = layout_preview,
};
