/* SPDX-License-Identifier: 0BSD */
/* Dialogs use the shared screen stack; the operation owns its timer. */
#include "apidemo.h"

#include <stdio.h>

enum {
	COMMAND_STAY = 1,
	COMMAND_LEAVE,
	COMMAND_CANCEL,
	COMMAND_SHOW,
};

#define STEP_MS 150
#define STEP 5
#define MAXIMUM 100
#define PREPARE_MS 1500

static size_t kind_item;

static void alert_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	struct apidemo *app = context;
	static const enum fplinux_ui_alert_kind kinds[] = {
		FPLINUX_UI_ALERT_INFO,
		FPLINUX_UI_ALERT_WARNING,
		FPLINUX_UI_ALERT_ERROR,
	};
	size_t selected = fplinux_ui_form_get_choice(form, kind_item);

	if (command_id == APIDEMO_COMMAND_BACK) {
		fplinux_ui_go_back(ui);
	} else if (command_id == COMMAND_SHOW &&
		   selected < APIDEMO_COUNT(kinds)) {
		apidemo_push(
			app,
			fplinux_ui_alert_new(
				ui, kinds[selected], NULL,
				_("This message stays until Close. Longer messages scroll "
				  "with Up and Down; Left and Right move by a page. "
				  "Closing the message returns to the same form."),
				0),
			"fplinux_ui_alert_new");
	}
}

static void open_alert(struct apidemo *app, const struct apidemo_entry *entry)
{
	const char *kinds[] = { _("Information"), _("Warning"), _("Error") };
	const struct fplinux_ui_command show = {
		.id = COMMAND_SHOW,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Show"),
		.icon = apidemo_glyph(app, "view"),
	};
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	kind_item = fplinux_ui_form_append_choice(form, _("Kind"),
						  FPLINUX_UI_CHOICE_POPUP,
						  kinds, APIDEMO_COUNT(kinds));
	fplinux_ui_form_append_string(
		form, NULL, _("Choose a kind, then Show to open its message."));
	fplinux_ui_screen_add_command(form, &show);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, alert_command, NULL, app);
	fplinux_ui_push(app->ui, form);
}

static void confirm_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *alert, int command_id,
			    void *context)
{
	const char *title;
	struct apidemo *app = context;

	(void)ui;
	(void)alert;
	app->decision = NULL;
	switch (command_id) {
	case COMMAND_STAY:
		title = _("Stay chosen");
		break;
	case COMMAND_LEAVE:
		title = _("Leave chosen");
		break;
	case COMMAND_CANCEL:
		title = _("Cancel chosen");
		break;
	default:
		return;
	}
	/* The runtime already removed the confirmation before its listener. */
	apidemo_show(context, title, _("The choice returned to the example."));
}

static void open_confirmation(struct apidemo *app,
			      const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command commands[] = {
		{ .id = COMMAND_STAY,
		  .type = FPLINUX_UI_COMMAND_OK,
		  .label = _("Stay"),
		  .icon = apidemo_glyph(app, "check") },
		{ .id = COMMAND_LEAVE,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = _("Leave"),
		  .icon = apidemo_glyph(app, "exit") },
		{ .id = COMMAND_CANCEL, .type = FPLINUX_UI_COMMAND_CANCEL },
	};
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_CONFIRMATION, _(entry->title),
		_("Leave without saving?"), 0);

	if (!alert) {
		apidemo_error(app, "fplinux_ui_alert_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(commands); i++)
		fplinux_ui_screen_add_command(alert, &commands[i]);
	fplinux_ui_screen_set_listener(alert, confirm_command, NULL, app);
	app->decision = alert;
	fplinux_ui_push(app->ui, alert);
}

static struct {
	struct apidemo *app;
	struct fplinux_ui_screen *alert;
	lv_timer_t *timer;
	unsigned int value;
} busy;

/* Cancel has already removed the alert; only autonomous completion pops it. */
static void stop_busy(bool pop)
{
	if (busy.timer)
		lv_timer_delete(busy.timer);
	busy.timer = NULL;
	if (pop && busy.alert && fplinux_ui_current(busy.app->ui) == busy.alert)
		fplinux_ui_pop(busy.app->ui);
	busy.alert = NULL;
}

static void progress_step(lv_timer_t *timer)
{
	char text[64];

	(void)timer;
	if (busy.value >= MAXIMUM) {
		struct apidemo *app = busy.app;

		stop_busy(true);
		apidemo_show(app, _("Finished"), _("The operation completed."));
		return;
	}
	busy.value += STEP;
	snprintf(text, sizeof(text), _("Completed: %u%%"), busy.value);
	fplinux_ui_alert_set_text(busy.alert, text);
	fplinux_ui_alert_set_progress(busy.alert, busy.value, MAXIMUM);
}

static void prepared(lv_timer_t *timer)
{
	fplinux_ui_alert_set_text(busy.alert, _("Working…"));
	fplinux_ui_alert_set_progress(busy.alert, 0, MAXIMUM);
	lv_timer_set_cb(timer, progress_step);
	lv_timer_set_period(timer, STEP_MS);
}

static void busy_command(struct fplinux_ui *ui, struct fplinux_ui_screen *alert,
			 int command_id, void *context)
{
	(void)ui;
	(void)alert;
	if (command_id != COMMAND_CANCEL)
		return;
	stop_busy(false);
	apidemo_show(context, _("Cancelled"), _("The operation stopped."));
}

static void open_busy(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command cancel = {
		.id = COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
	};
	struct fplinux_ui_screen *alert =
		fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_INFO,
				     _(entry->title), _("Preparing…"), 0);
	lv_timer_t *timer;

	if (!alert) {
		apidemo_error(app, "fplinux_ui_alert_new");
		return;
	}
	fplinux_ui_screen_add_command(alert, &cancel);
	fplinux_ui_screen_set_listener(alert, busy_command, NULL, app);
	fplinux_ui_push(app->ui, alert);
	if (!fplinux_ui_alert_set_progress(alert, 0, 0)) {
		fplinux_ui_pop(app->ui);
		apidemo_error(app, "fplinux_ui_alert_set_progress");
		return;
	}
	timer = lv_timer_create(prepared, PREPARE_MS, NULL);
	if (!timer) {
		fplinux_ui_pop(app->ui);
		apidemo_error(app, "lv_timer_create");
		return;
	}
	busy.app = app;
	busy.alert = alert;
	busy.timer = timer;
	busy.value = 0;
	if (!app->visible)
		lv_timer_pause(timer);
}

static void alerts_visibility(struct apidemo *app, bool visible)
{
	(void)app;
	if (!busy.timer)
		return;
	if (visible)
		lv_timer_resume(busy.timer);
	else
		lv_timer_pause(busy.timer);
}

static void alerts_cleanup(struct apidemo *app)
{
	(void)app;
	stop_busy(false);
}

static const struct apidemo_entry entries[] = {
	{ N_("Alert"),
	  "fplinux_ui_alert_new · FPLINUX_UI_ALERT_INFO · "
	  "FPLINUX_UI_ALERT_WARNING · FPLINUX_UI_ALERT_ERROR",
	  open_alert, 0, N_("Show and scroll a message") },
	{ N_("Confirmation"),
	  "FPLINUX_UI_ALERT_CONFIRMATION · "
	  "FPLINUX_UI_COMMAND_OK · FPLINUX_UI_COMMAND_CANCEL",
	  open_confirmation, 0, N_("Choose Stay, Leave or Cancel") },
	{ N_("Progress"),
	  "fplinux_ui_alert_set_progress · fplinux_ui_alert_set_text",
	  open_busy, 0, N_("Watch an operation or cancel it") },
};

const struct apidemo_category apidemo_alerts = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = alerts_visibility,
	.cleanup = alerts_cleanup,
};
