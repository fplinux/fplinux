/* SPDX-License-Identifier: 0BSD */
#include "apidemo-forms.h"

/* The progress bar fills in five seconds while its timer runs. */
#define PROGRESS_MAXIMUM 100
#define PROGRESS_STEP 2
#define PROGRESS_STEP_MS 100

static struct fplinux_ui_screen *gauges_form;
static size_t gauges_status;
static size_t gauge_small;
static size_t gauge_large;
static size_t gauge_progress;
static lv_timer_t *progress_timer;

static void stop_progress(void)
{
	if (progress_timer)
		lv_timer_delete(progress_timer);
	progress_timer = NULL;
}

/* OK starts or stops the progress bar; the label says which. */
static void show_start(struct apidemo *app, struct fplinux_ui_screen *form)
{
	apidemo_forms_add_command(form, COMMAND_START, FPLINUX_UI_COMMAND_OK,
				  progress_timer ? _("Stop") : _("Start"), 0,
				  apidemo_glyph(app, progress_timer ? "stop" :
								      "play"));
}

/* The bar stops at its maximum; the timer goes with it. */
static void progress_step(lv_timer_t *timer)
{
	struct apidemo *app = lv_timer_get_user_data(timer);
	int value = fplinux_ui_form_get_value(gauges_form, gauge_progress);

	fplinux_ui_form_set_value(gauges_form, gauge_progress,
				  value + PROGRESS_STEP);
	if (value + PROGRESS_STEP < PROGRESS_MAXIMUM)
		return;
	stop_progress();
	show_start(app, gauges_form);
}

static void toggle_progress(struct apidemo *app, struct fplinux_ui_screen *form)
{
	if (progress_timer) {
		stop_progress();
	} else {
		if (fplinux_ui_form_get_value(form, gauge_progress) >=
		    PROGRESS_MAXIMUM)
			fplinux_ui_form_set_value(form, gauge_progress, 0);
		progress_timer =
			lv_timer_create(progress_step, PROGRESS_STEP_MS, app);
	}
	show_start(app, form);
}

/* An interactive gauge reports every step of Left and Right. */
static void gauges_changed(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *form, size_t index,
			   void *context)
{
	(void)ui;
	(void)context;
	apidemo_forms_show_status(form, gauges_status, _("Item %zu: %d"), index,
				  fplinux_ui_form_get_value(form, index));
}

static void gauges_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *form, int command_id,
			   void *context)
{
	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		/* The timer must not outlive the screen. */
		stop_progress();
		gauges_form = NULL;
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_START:
		toggle_progress(context, form);
		break;
	case COMMAND_SHOW:
		apidemo_showf(context, _("Values"),
			      "get_value(%zu) = %d\nget_value(%zu) = %d\n"
			      "get_value(%zu) = %d",
			      gauge_small,
			      fplinux_ui_form_get_value(form, gauge_small),
			      gauge_large,
			      fplinux_ui_form_get_value(form, gauge_large),
			      gauge_progress,
			      fplinux_ui_form_get_value(form, gauge_progress));
		break;
	case COMMAND_RESET:
		fplinux_ui_form_set_value(form, gauge_small, 5);
		fplinux_ui_form_set_value(form, gauge_large, 500);
		fplinux_ui_form_set_value(form, gauge_progress, 0);
		break;
	default:
		break;
	}
}

void apidemo_forms_open_gauges(struct apidemo *app,
			       const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	gauges_form = form;
	gauges_status = apidemo_forms_append_status(form);
	/* A range of up to 20 moves by 1, a longer one by a twentieth. */
	gauge_small = fplinux_ui_form_append_gauge(form, _("0–10, step 1"), 0,
						   10, 5, true);
	gauge_large = fplinux_ui_form_append_gauge(form, _("0–1000, step 50"),
						   0, 1000, 500, true);
	/* Not interactive: a progress bar that the application moves. */
	gauge_progress = fplinux_ui_form_append_gauge(
		form, _("Progress"), 0, PROGRESS_MAXIMUM, 0, false);
	show_start(app, form);
	apidemo_forms_add_command(form, COMMAND_SHOW, FPLINUX_UI_COMMAND_SCREEN,
				  _("Show values"), 1,
				  apidemo_glyph(app, "view"));
	apidemo_forms_add_command(form, COMMAND_RESET,
				  FPLINUX_UI_COMMAND_SCREEN, _("Reset"), 2,
				  apidemo_glyph(app, "refresh"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, gauges_command, gauges_changed,
				       app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}

/* Category hooks: the progress timer runs only while it is seen. */

void apidemo_forms_visibility(struct apidemo *app, bool visible)
{
	(void)app;
	if (!progress_timer)
		return;
	if (visible)
		lv_timer_resume(progress_timer);
	else
		lv_timer_pause(progress_timer);
}

void apidemo_forms_cleanup(struct apidemo *app)
{
	(void)app;
	stop_progress();
	gauges_form = NULL;
}
