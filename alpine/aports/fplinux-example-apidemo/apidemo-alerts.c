/* SPDX-License-Identifier: 0BSD */
/*
 * Alerts: kinds, titles, timeouts, confirmations, busy and progress
 * indicators, and text that changes.
 *
 * Every demo here pushes its alert straight from its row, over the category
 * list. An alert has its own title instead of a title band, so these alerts
 * carry titles the way an application would write them ("Battery low"),
 * while the row says what the demo shows.
 */
#include "apidemo.h"

#include <errno.h>
#include <stdio.h>

/* Command ids of the alerts in this file. */
enum {
	COMMAND_STAY = 1,
	COMMAND_LEAVE,
	COMMAND_CANCEL,
};

/* The application pops the spinner after three seconds. */
#define SPINNER_MS 3000
/* The cancellable spinner stands for six seconds of work. */
#define WORK_MS 6000
/* The bar moves by STEP of MAXIMUM every STEP_MS: three seconds in all. */
#define STEP_MS 150
#define STEP 5
#define MAXIMUM 100
/* The spinner of "Spinner, then a bar" turns for two seconds. */
#define PREPARE_MS 2000
/* "Text while shown" copies one file a second. */
#define FILES 3
#define FILE_MS 1000
/* "Text set before showing" changes its text every 1.5 s. */
#define TEXT_STEP_MS 1500

enum variant {
	VARIANT_SPINNER,
	VARIANT_SPINNER_CANCEL,
	VARIANT_BAR,
	VARIANT_SPINNER_BAR,
	VARIANT_FILES,
	VARIANT_TEXT_BEFORE,
};

/*
 * The busy alert on show and the timer that stands for its work. There is
 * at most one: a busy alert covers the list, so no other demo can start
 * while it is shown.
 */
static struct {
	struct apidemo *app;
	struct fplinux_ui_screen *alert;
	lv_timer_t *timer;
	int value;
} busy;

/*
 * Ends the work. pop is true when the work finished by itself: the alert is
 * still on the stack and the application removes it. When the user chose
 * Cancel, the runtime has removed the alert already and deletes it after
 * the listener, so the application only forgets it.
 */
static void stop_busy(bool pop)
{
	if (busy.timer)
		lv_timer_delete(busy.timer);
	busy.timer = NULL;
	/* fplinux_ui_pop() removes the top screen: check that it is ours. */
	if (pop && busy.alert && fplinux_ui_current(busy.app->ui) == busy.alert)
		fplinux_ui_pop(busy.app->ui);
	busy.alert = NULL;
}

/* The spinner's work is done. */
static void spinner_done(lv_timer_t *timer)
{
	(void)timer;
	stop_busy(true);
}

/* One step of the bar; it stays full for one step before it closes. */
static void progress_step(lv_timer_t *timer)
{
	(void)timer;
	if (busy.value >= MAXIMUM) {
		stop_busy(true);
		return;
	}
	busy.value += STEP;
	fplinux_ui_alert_set_progress(busy.alert, busy.value, MAXIMUM);
}

/* Preparation ends: the same alert turns from a spinner into a bar. */
static void prepared(lv_timer_t *timer)
{
	busy.value = 0;
	fplinux_ui_alert_set_progress(busy.alert, busy.value, MAXIMUM);
	lv_timer_set_cb(timer, progress_step);
	lv_timer_set_period(timer, STEP_MS);
}

/* The text of the file being copied, or of the end, after done files. */
static void files_text(char *text, size_t size, int done)
{
	if (done < FILES)
		snprintf(text, size, _("Copying file %d of %d."), done + 1,
			 FILES);
	else
		snprintf(text, size,
			 ngettext("All %d file copied.", "All %d files copied.",
				  FILES),
			 FILES);
}

/*
 * One file copied: the bar moves and the shown alert takes a new text,
 * which the box fits as it did at creation. The last text stays for one
 * step before the alert closes.
 */
static void file_step(lv_timer_t *timer)
{
	char text[64];

	(void)timer;
	if (busy.value >= FILES) {
		stop_busy(true);
		return;
	}
	++busy.value;
	fplinux_ui_alert_set_progress(busy.alert, busy.value, FILES);
	files_text(text, sizeof(text), busy.value);
	fplinux_ui_alert_set_text(busy.alert, text);
}

/*
 * NULL and "" leave the alert without text: the box holds the title and
 * the spinner. A text brings the line back; the alert closes at the end.
 */
static void text_step(lv_timer_t *timer)
{
	(void)timer;
	switch (++busy.value) {
	case 1:
		fplinux_ui_alert_set_text(busy.alert, NULL);
		break;
	case 2:
		fplinux_ui_alert_set_text(
			busy.alert, _("A text again. Next, \"\" clears it."));
		break;
	case 3:
		fplinux_ui_alert_set_text(busy.alert, "");
		break;
	default:
		stop_busy(true);
		break;
	}
}

/*
 * Cancel removed the alert before this call. Stop the work and forget the
 * alert: neither pop it nor update it, the runtime deletes it after the
 * call. The listener may push a screen of its own.
 */
static void busy_command(struct fplinux_ui *ui, struct fplinux_ui_screen *alert,
			 int command_id, void *context)
{
	struct apidemo *app = context;

	(void)ui;
	(void)alert;
	if (command_id != COMMAND_CANCEL)
		return;
	stop_busy(false);
	apidemo_show(app, _("Cancelled"),
		     _("The listener stopped the work and forgot the alert. "
		       "The runtime deletes the alert after the call."));
}

/*
 * Pushes an information alert that shows a busy indicator: a spinner for
 * maximum 0, otherwise a bar. The timer stands for the work and calls step
 * every period_ms. A busy alert has no Close: without a Cancel command only
 * the application can remove it, so the work must be sure to end. Keep the
 * text short: the indicator goes below it, and on the smallest surface,
 * 128 x 96, two lines of text leave room for it. shown, when not NULL,
 * replaces the text before the alert shows.
 */
static void start_busy(struct apidemo *app, const char *title, const char *text,
		       const char *shown, int maximum, bool cancel,
		       lv_timer_cb_t step, uint32_t period_ms)
{
	const struct fplinux_ui_command cancel_command = {
		.id = COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
	};
	struct fplinux_ui_screen *alert;
	lv_timer_t *timer = lv_timer_create(step, period_ms, NULL);

	if (!timer) {
		apidemo_error(app, "lv_timer_create");
		return;
	}
	alert = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_INFO, title,
				     text, 0);
	if (alert && shown)
		fplinux_ui_alert_set_text(alert, shown);
	if (!apidemo_push(app, alert, "fplinux_ui_alert_new")) {
		lv_timer_delete(timer);
		return;
	}
	/* The alert is on top: the spinner turns at once. */
	if (!fplinux_ui_alert_set_progress(alert, 0, maximum)) {
		lv_timer_delete(timer);
		apidemo_error(app, "fplinux_ui_alert_set_progress");
		return;
	}
	if (cancel) {
		fplinux_ui_screen_add_command(alert, &cancel_command);
		fplinux_ui_screen_set_listener(alert, busy_command, NULL, app);
	}
	busy.app = app;
	busy.alert = alert;
	busy.timer = timer;
	busy.value = 0;
	if (!app->visible)
		lv_timer_pause(timer);
}

static void open_busy(struct apidemo *app, const struct apidemo_entry *entry)
{
	char text[64];

	switch (entry->variant) {
	case VARIANT_SPINNER:
		start_busy(app, _("Searching"), _("Closes in 3\u00a0s."), NULL,
			   0, false, spinner_done, SPINNER_MS);
		break;
	case VARIANT_SPINNER_CANCEL:
		start_busy(app, _("Connecting"), _("Ends in 6\u00a0s."), NULL,
			   0, true, spinner_done, WORK_MS);
		break;
	case VARIANT_BAR:
		start_busy(app, _("Copying"), _("Copying 20 files."), NULL,
			   MAXIMUM, true, progress_step, STEP_MS);
		break;
	case VARIANT_SPINNER_BAR:
		start_busy(app, _("Downloading"), _("Preparing, then a bar."),
			   NULL, 0, true, prepared, PREPARE_MS);
		break;
	case VARIANT_FILES:
		files_text(text, sizeof(text), 0);
		start_busy(app, _("Copying"), text, NULL, FILES, true,
			   file_step, FILE_MS);
		break;
	case VARIANT_TEXT_BEFORE:
		/* The text given to the constructor never shows. */
		start_busy(app, _("Preparing"),
			   _("The constructor's text, replaced at once."),
			   _("Set before the alert showed."), 0, true,
			   text_step, TEXT_STEP_MS);
		break;
	default:
		break;
	}
}

/* The kinds with a title of the application's own. */
static void open_kind(struct apidemo *app, const struct apidemo_entry *entry)
{
	const char *title = NULL;
	const char *text = NULL;

	switch (entry->variant) {
	case FPLINUX_UI_ALERT_INFO:
		title = _("Message sent");
		text = _("An information alert is framed in the focus colour. "
			 "With timeout 0 it stays until you close it.");
		break;
	case FPLINUX_UI_ALERT_WARNING:
		title = _("Battery low");
		text = _("A warning takes the warning colour for its frame and "
			 "title.");
		break;
	case FPLINUX_UI_ALERT_ERROR:
		title = _("Not saved");
		text = _("An error takes the danger colour for its frame and "
			 "title.");
		break;
	default:
		return;
	}
	apidemo_push(
		app,
		fplinux_ui_alert_new(app->ui,
				     (enum fplinux_ui_alert_kind)entry->variant,
				     title, text, 0),
		"fplinux_ui_alert_new");
}

/*
 * A NULL title shows the kind's word in the system language. The alerts
 * are pushed in reverse, so Information is on top and each Close shows the
 * next kind.
 */
static void open_kind_words(struct apidemo *app,
			    const struct apidemo_entry *entry)
{
	static const enum fplinux_ui_alert_kind kinds[] = {
		FPLINUX_UI_ALERT_ERROR,
		FPLINUX_UI_ALERT_WARNING,
		FPLINUX_UI_ALERT_CONFIRMATION,
		FPLINUX_UI_ALERT_INFO,
	};

	(void)entry;
	for (size_t i = 0; i < APIDEMO_COUNT(kinds); i++) {
		const char *text =
			i ? _("The title is NULL, so the alert shows the word "
			      "of its kind. Close shows the next one.") :
			    _("The title is NULL, so the alert shows the word "
			      "of its kind. This is the last one.");

		if (!apidemo_push(app,
				  fplinux_ui_alert_new(app->ui, kinds[i], NULL,
						       text, 0),
				  "fplinux_ui_alert_new"))
			return;
	}
}

/* A timed alert closes itself and never calls a listener. */
static void open_timed(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *alert;

	if (entry->variant == FPLINUX_UI_ALERT_WARNING)
		alert = fplinux_ui_alert_new(
			app->ui, FPLINUX_UI_ALERT_WARNING, _("Connection lost"),
			_("This warning closes itself after 4\u00a0s. The time runs "
			  "only while it is on top and visible."),
			4000);
	else
		alert = fplinux_ui_alert_new(
			app->ui, FPLINUX_UI_ALERT_INFO, _("Saved"),
			_("This alert closes itself after 1.5\u00a0s."), 1500);
	apidemo_push(app, alert, "fplinux_ui_alert_new");
}

static void open_long_text(struct apidemo *app,
			   const struct apidemo_entry *entry)
{
	apidemo_push(
		app,
		fplinux_ui_alert_new(
			app->ui, FPLINUX_UI_ALERT_INFO, _(entry->title),
			_("An alert grows with its text up to the surface "
			  "less its margins. A longer text scrolls: Up and "
			  "Down by a line, Left and Right by a page.\n\nKeep "
			  "alerts short anyway: at 128 pixels a line holds "
			  "about sixteen letters, and four lines fit without "
			  "scrolling.\n\nThe soft keys stay the same while "
			  "the text scrolls."),
			0),
		"fplinux_ui_alert_new");
}

/*
 * The Settings pattern for a question that would discard work: OK keeps the
 * safe choice, the left soft key repeats the question's verb and Cancel is
 * on the right.
 */
static void confirm_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *alert, int command_id,
			    void *context)
{
	struct apidemo *app = context;
	const char *title;

	(void)ui;
	(void)alert;
	/* The alert has already left the stack: the list is on top again. */
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
	apidemo_show(app, title,
		     _("The alert left the stack before its listener ran. "
		       "The listener must not pop it; the runtime deletes it "
		       "afterwards."));
}

static void open_confirmation(struct apidemo *app,
			      const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command commands[] = {
		{ .id = COMMAND_STAY,
		  .type = FPLINUX_UI_COMMAND_OK,
		  .label = _("Stay") },
		{ .id = COMMAND_LEAVE,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = _("Leave") },
		/* NULL: the runtime's Cancel in the system language. */
		{ .id = COMMAND_CANCEL, .type = FPLINUX_UI_COMMAND_CANCEL },
	};
	/* NULL title: the kind's word, Confirmation. */
	struct fplinux_ui_screen *alert =
		fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_CONFIRMATION,
				     NULL, _("Leave without saving?"), 0);

	(void)entry;
	if (!alert) {
		apidemo_error(app, "fplinux_ui_alert_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(commands); i++)
		fplinux_ui_screen_add_command(alert, &commands[i]);
	fplinux_ui_screen_set_listener(alert, confirm_command, NULL, app);
	apidemo_push(app, alert, "fplinux_ui_alert_new");
}

/*
 * The value is clamped to 0 .. maximum. A busy alert offers no Close of its
 * own, so this one gets a CANCEL command labelled Close.
 */
static void open_clamped(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command close = {
		.id = COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.label = _("Close"),
	};
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_INFO, _("Clamped"),
		_("Value 150, maximum 100."), 0);

	(void)entry;
	if (!apidemo_push(app, alert, "fplinux_ui_alert_new"))
		return;
	fplinux_ui_screen_add_command(alert, &close);
	if (!fplinux_ui_alert_set_progress(alert, 150, MAXIMUM))
		apidemo_error(app, "fplinux_ui_alert_set_progress");
}

/* "true", or the errno name of a call that returned false. */
static const char *outcome(bool ok, int error)
{
	return ok ? "true" : apidemo_errno_name(error);
}

/*
 * fplinux_ui_alert_set_progress() refuses a screen that is not an alert and
 * a negative maximum with EINVAL, and changes nothing.
 */
static void open_progress_errors(struct apidemo *app,
				 const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *alert;
	bool list_ok;
	bool negative_ok;
	int list_error;
	int negative_error;

	/* The list that opened this demo is on top: not an alert. */
	list_ok = fplinux_ui_alert_set_progress(fplinux_ui_current(app->ui), 0,
						0);
	list_error = errno;
	alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_INFO, _("Not busy"),
		_("This alert refused a negative maximum, so it is still a "
		  "plain alert with Close."),
		0);
	if (!apidemo_push(app, alert, "fplinux_ui_alert_new"))
		return;
	negative_ok = fplinux_ui_alert_set_progress(alert, 0, -1);
	negative_error = errno;
	apidemo_showf(app, _(entry->title),
		      _("On the list: %s.\nMaximum −1: %s."),
		      outcome(list_ok, list_error),
		      outcome(negative_ok, negative_error));
}

/* fplinux_ui_alert_set_text() refuses another screen and NULL with EINVAL. */
static void open_text_errors(struct apidemo *app,
			     const struct apidemo_entry *entry)
{
	bool list_ok;
	bool null_ok;
	int list_error;
	int null_error;

	/* The list that opened this demo is on top: not an alert. */
	errno = 0;
	list_ok = fplinux_ui_alert_set_text(fplinux_ui_current(app->ui),
					    _("Text"));
	list_error = errno;
	errno = 0;
	null_ok = fplinux_ui_alert_set_text(NULL, _("Text"));
	null_error = errno;
	apidemo_showf(app, _(entry->title), _("On the list: %s.\nOn NULL: %s."),
		      outcome(list_ok, list_error),
		      outcome(null_ok, null_error));
}

static const struct apidemo_entry entries[] = {
	{ N_("Information"),
	  "fplinux_ui_alert_new · FPLINUX_UI_ALERT_INFO · timeout_ms 0",
	  open_kind, FPLINUX_UI_ALERT_INFO },
	{ N_("Warning"), "fplinux_ui_alert_new · FPLINUX_UI_ALERT_WARNING",
	  open_kind, FPLINUX_UI_ALERT_WARNING },
	{ N_("Error"), "fplinux_ui_alert_new · FPLINUX_UI_ALERT_ERROR",
	  open_kind, FPLINUX_UI_ALERT_ERROR },
	{ N_("Confirmation"),
	  "FPLINUX_UI_ALERT_CONFIRMATION · OK, SCREEN, CANCEL",
	  open_confirmation, 0 },
	{ N_("Title from the kind"), "fplinux_ui_alert_new · title NULL",
	  open_kind_words, 0 },
	{ N_("Closes after 1.5\u00a0s"),
	  "fplinux_ui_alert_new · timeout_ms 1500", open_timed,
	  FPLINUX_UI_ALERT_INFO },
	{ N_("Closes after 4\u00a0s"),
	  "FPLINUX_UI_ALERT_WARNING · timeout_ms 4000", open_timed,
	  FPLINUX_UI_ALERT_WARNING },
	{ N_("Long text"), "fplinux_ui_alert_new · scrolls with the D-pad",
	  open_long_text, 0 },
	{ N_("Spinner"),
	  "fplinux_ui_alert_set_progress · maximum 0 · fplinux_ui_pop",
	  open_busy, VARIANT_SPINNER },
	{ N_("Spinner with Cancel"),
	  "fplinux_ui_alert_set_progress · FPLINUX_UI_COMMAND_CANCEL",
	  open_busy, VARIANT_SPINNER_CANCEL },
	{ N_("Progress bar"), "fplinux_ui_alert_set_progress · value, maximum",
	  open_busy, VARIANT_BAR },
	{ N_("Value above maximum"),
	  "fplinux_ui_alert_set_progress(alert, 150, 100)", open_clamped, 0 },
	{ N_("Spinner, then a bar"),
	  "fplinux_ui_alert_set_progress · maximum 0, then 100", open_busy,
	  VARIANT_SPINNER_BAR },
	{ N_("Progress errors"), "fplinux_ui_alert_set_progress · EINVAL",
	  open_progress_errors, 0 },
	{ N_("Text while shown"),
	  "fplinux_ui_alert_set_text · progress messages", open_busy,
	  VARIANT_FILES },
	{ N_("Text set before showing"),
	  "fplinux_ui_alert_set_text · before push · NULL · \"\"", open_busy,
	  VARIANT_TEXT_BEFORE },
	{ N_("Text errors"), "fplinux_ui_alert_set_text · EINVAL",
	  open_text_errors, 0 },
};

/* The work waits while the application is hidden, as the spinner does. */
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

/* Screens may still be on the stack: forget the alert, delete the timer. */
static void alerts_cleanup(struct apidemo *app)
{
	(void)app;
	stop_busy(false);
}

const struct apidemo_category apidemo_alerts = {
	.title = N_("Alerts"),
	.icon = APIDEMO_IMAGE_DIR "alerts-symbolic.png",
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = alerts_visibility,
	.cleanup = alerts_cleanup,
};
