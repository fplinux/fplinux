/* SPDX-License-Identifier: 0BSD */
/* RGB565 frames: synchronous drawing and painting across timer turns. */
#include "apidemo.h"
#include <fplinux/fplinux-ui-frame.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

enum frame_command {
	FRAME_FULL = 1,
	FRAME_RUN,
	FRAME_MODE,
	FRAME_ERRORS,
	FRAME_HELP,
};

static struct {
	struct apidemo *app;
	struct fplinux_ui_screen *screen;
	struct fplinux_ui_frame *frame;
	lv_timer_t *timer;
	lv_obj_t *label;
	lv_obj_t *owner;
	bool staged;
	bool painting;
	bool enabled;
	unsigned int completed;
	int width;
	int height;
	int x;
} software;

static void readout(void)
{
	lv_label_set_text_fmt(software.label, _("Completed frames: %u"),
			      software.completed);
}

static void full_frame(void)
{
	uint16_t *pixels = fplinux_ui_frame_pixels(
		software.frame, &software.width, &software.height);

	for (int i = 0; i < software.width * software.height; ++i)
		pixels[i] = 0x1082;
	software.x = 0;
	software.painting = false;
	fplinux_ui_frame_damage(software.frame, NULL);
	if (fplinux_ui_frame_present(software.frame))
		software.completed++;
	readout();
}

/* Marks only the rectangle written here; coordinates are inclusive. */
static void rectangle(int x, int top, int bottom, uint16_t color)
{
	uint16_t *pixels = fplinux_ui_frame_pixels(software.frame, NULL, NULL);
	int right = x + 15 < software.width ? x + 15 : software.width - 1;
	lv_area_t area = { x, top, right, bottom };

	if (area.y2 >= software.height)
		area.y2 = software.height - 1;
	for (int y = area.y1; y <= area.y2; ++y)
		for (int column = area.x1; column <= area.x2; ++column)
			pixels[y * software.width + column] = color;
	fplinux_ui_frame_damage(software.frame, &area);
}

static void frame_tick(lv_timer_t *timer)
{
	(void)timer;
	if (!software.painting) {
		rectangle(software.x, 36, 51, 0x1082);
		software.x =
			software.x + 16 < software.width ? software.x + 16 : 0;
		rectangle(software.x, 36, software.staged ? 43 : 51, 0xf800);
		if (software.staged) {
			/* A system-menu redraw now must still see the completed frame. */
			software.painting = true;
			return;
		}
	} else {
		rectangle(software.x, 44, 51, 0xf800);
		software.painting = false;
	}
	if (fplinux_ui_frame_present(software.frame))
		software.completed++;
	readout();
}

static void null_errors(struct apidemo *app)
{
	int errors[9];

	fplinux_ui_frame_new(NULL, 1, 1, FPLINUX_UI_FRAME_DIRECT);
	errors[0] = errno;
	fplinux_ui_frame_object(NULL);
	errors[1] = errno;
	fplinux_ui_frame_pixels(NULL, NULL, NULL);
	errors[2] = errno;
	fplinux_ui_frame_resize(NULL, 1, 1);
	errors[3] = errno;
	fplinux_ui_frame_damage(NULL, NULL);
	errors[4] = errno;
	fplinux_ui_frame_present(NULL);
	errors[5] = errno;
	fplinux_ui_frame_set_timer(NULL, NULL, false);
	errors[6] = errno;
	fplinux_ui_frame_set_timer(software.frame, NULL, true);
	errors[7] = errno;
	fplinux_ui_frame_resize(software.frame, 0, 1);
	errors[8] = errno;
	fplinux_ui_frame_delete(NULL); /* The documented harmless variant. */
	apidemo_showf(
		app, _("Frame errors"),
		_("NULL new: %1$s\nNULL object: %2$s\nNULL pixels: %3$s\n"
		  "NULL resize: %4$s\nNULL damage: %5$s\nNULL present: %6$s\n"
		  "NULL timer owner: %7$s\nNULL enabled timer: %8$s\n"
		  "Zero width: %9$s\nNULL delete: OK"),
		apidemo_errno_name(errors[0]), apidemo_errno_name(errors[1]),
		apidemo_errno_name(errors[2]), apidemo_errno_name(errors[3]),
		apidemo_errno_name(errors[4]), apidemo_errno_name(errors[5]),
		apidemo_errno_name(errors[6]), apidemo_errno_name(errors[7]),
		apidemo_errno_name(errors[8]));
}

static void frame_help(struct apidemo *app)
{
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_INFO, _("Software frames"),
		_("The red square changes only its old and new rectangles. "
		  "Options > Full repaint marks the whole buffer.\n\n"
		  "Direct drawing uses one buffer and finishes in one timer call. "
		  "Interrupted painting uses two: one call draws half a square, "
		  "the next finishes and publishes it. A system menu opened "
		  "between calls still sees the last completed square.\n\n"
		  "Lock the phone or switch applications: the registered timer "
		  "pauses automatically. Return to resume. Pause animation keeps "
		  "it stopped even after hiding and showing.\n\n"
		  "Full screen changes the buffer size on RESIZE, clears old "
		  "damage and draws a complete frame. Back restores normal view."),
		0);

	if (!alert) {
		apidemo_error(app, "fplinux_ui_alert_new");
		return;
	}
	fplinux_ui_navigate(app->ui, alert);
}

static void label_run(void)
{
	const struct fplinux_ui_command command = {
		.id = FRAME_RUN,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = software.enabled ? _("Pause animation") :
					    _("Resume animation"),
	};

	fplinux_ui_screen_add_command(software.screen, &command);
}

static void frame_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int id,
			  void *context)
{
	struct apidemo *app = context;

	(void)screen;
	if (id == FRAME_FULL) {
		full_frame();
	} else if (id == FRAME_RUN) {
		software.enabled = !software.enabled;
		fplinux_ui_frame_set_timer(software.frame, software.timer,
					   software.enabled);
		label_run();
	} else if (id == FRAME_MODE) {
		if (!fplinux_ui_set_fullscreen(ui,
					       !fplinux_ui_is_fullscreen(ui)))
			apidemo_error(app, "fplinux_ui_set_fullscreen");
	} else if (id == FRAME_ERRORS) {
		null_errors(app);
	} else if (id == FRAME_HELP) {
		frame_help(app);
	} else if (id == APIDEMO_COMMAND_BACK) {
		if (!fplinux_ui_set_fullscreen(ui, false))
			perror("fplinux_ui_set_fullscreen");
		fplinux_ui_go_back(ui);
	}
}

static void frame_cleanup(struct apidemo *app)
{
	(void)app;
	if (software.timer) {
		fplinux_ui_frame_set_timer(software.frame, NULL, false);
		lv_timer_delete(software.timer);
		software.timer = NULL;
	}
	fplinux_ui_frame_delete(software.frame);
	software.frame = NULL;
	software.screen = NULL;
	software.owner = NULL;
	software.label = NULL;
}

static void frame_deleted(lv_event_t *event)
{
	(void)event;
	frame_cleanup(software.app);
}

static void frame_resize(struct apidemo *app)
{
	int width, height;

	if (!software.screen)
		return;
	if (!fplinux_ui_screen_get_content_size(software.screen, &width,
						&height) ||
	    !fplinux_ui_frame_resize(software.frame, width, height)) {
		apidemo_error(app, "fplinux_ui_frame_resize");
		return;
	}
	lv_obj_set_size(software.owner, width, height);
	lv_obj_set_width(software.label, width - 2 * app->metrics->margin);
	full_frame();
}

static void open_frame(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command commands[] = {
		{ .id = FRAME_FULL,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = _("Full repaint") },
		{ .id = FRAME_MODE,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = _("Full screen / normal") },
		{ .id = FRAME_ERRORS,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = _("Frame errors") },
		{ .id = FRAME_HELP,
		  .type = FPLINUX_UI_COMMAND_HELP,
		  .label = _("How to try frames") },
	};

	memset(&software, 0, sizeof(software));
	software.app = app;
	software.enabled = true;
	software.staged = entry->variant != 0;
	software.frame = fplinux_ui_frame_new(
		app->ui, fplinux_ui_width(app->ui), fplinux_ui_height(app->ui),
		software.staged ? FPLINUX_UI_FRAME_STAGED :
				  FPLINUX_UI_FRAME_DIRECT);
	if (!software.frame) {
		apidemo_error(app, "fplinux_ui_frame_new");
		return;
	}
	software.timer = lv_timer_create(frame_tick, 140, NULL);
	if (!software.timer) {
		frame_cleanup(app);
		errno = ENOMEM;
		apidemo_error(app, "lv_timer_create");
		return;
	}
	fplinux_ui_frame_set_timer(software.frame, software.timer, true);
	/* Check the owner callback before creating an unstacked screen. */
	software.owner = lv_obj_create(fplinux_ui_screen(app->ui));
	if (!software.owner) {
		frame_cleanup(app);
		errno = ENOMEM;
		apidemo_error(app, "lv_obj_create");
		return;
	}
	lv_obj_remove_style_all(software.owner);
	lv_obj_set_pos(software.owner, 0, 0);
	lv_obj_set_scrollable(software.owner, false);
	if (!lv_obj_add_event_cb(software.owner, frame_deleted, LV_EVENT_DELETE,
				 NULL)) {
		lv_obj_delete(software.owner);
		frame_cleanup(app);
		errno = ENOMEM;
		apidemo_error(app, "lv_obj_add_event_cb");
		return;
	}
	lv_obj_set_parent(fplinux_ui_frame_object(software.frame),
			  software.owner);
	software.label = lv_label_create(software.owner);
	if (!software.label) {
		lv_obj_delete(software.owner);
		errno = ENOMEM;
		apidemo_error(app, "lv_label_create");
		return;
	}
	lv_obj_set_pos(software.label, app->metrics->margin,
		       app->metrics->spacing);
	lv_obj_set_style_text_font(software.label, app->theme->font, 0);
	lv_obj_set_style_text_color(software.label, lv_color_white(), 0);
	software.screen = fplinux_ui_canvas_new(app->ui, _(entry->title));
	if (!software.screen) {
		int error = errno;

		lv_obj_delete(software.owner);
		errno = error;
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	lv_obj_set_parent(software.owner,
			  fplinux_ui_canvas_object(software.screen));
	apidemo_add_back(software.screen);
	for (size_t i = 0; i < APIDEMO_COUNT(commands); ++i)
		fplinux_ui_screen_add_command(software.screen, &commands[i]);
	label_run();
	fplinux_ui_screen_set_listener(software.screen, frame_command, NULL,
				       app);
	apidemo_push(app, software.screen, "fplinux_ui_canvas_new");
	frame_resize(app);
}

static const struct apidemo_entry entries[] = {
	{ N_("Direct drawing"), "fplinux_ui_frame_* · DIRECT · one buffer",
	  open_frame, 0 },
	{ N_("Interrupted painting"),
	  "fplinux_ui_frame_* · STAGED · completed frames", open_frame, 1 },
};

const struct apidemo_category apidemo_frame = {
	.title = N_("Software frames"),
	.icon = APIDEMO_IMAGE_DIR "canvas-symbolic.png",
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.cleanup = frame_cleanup,
	.resize = frame_resize,
};
